/*
 * MicroHH
 * Copyright (c) 2011-2020 Chiel van Heerwaarden
 * Copyright (c) 2011-2020 Thijs Heus
 * Copyright (c) 2014-2020 Bart van Stratum
 *
 * This file is part of MicroHH
 *
 * MicroHH is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * MicroHH is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with MicroHH.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdexcept>

#include "radiation_prescribed.h"
#include "grid.h"
#include "constants.h"
#include "input.h"
#include "timedep.h"
#include "stats.h"

template<typename TF>
Radiation_prescribed<TF>::Radiation_prescribed(
        Master& masterin, Grid<TF>& gridin, Fields<TF>& fieldsin, Input& inputin) :
        Radiation<TF>(masterin, gridin, fieldsin, inputin)
{
    swradiation = "prescribed";

    // Option for time varying prescribed fluxes through the `case_input.nc` file.
    swtimedep_prescribed = inputin.get_item<bool>("radiation", "swtimedep_prescribed", "", false);

    tdep_sw_flux_dn = std::make_unique<Timedep<TF>>(master, grid, "sw_flux_dn", swtimedep_prescribed);
    tdep_sw_flux_up = std::make_unique<Timedep<TF>>(master, grid, "sw_flux_up", swtimedep_prescribed);
    tdep_lw_flux_dn = std::make_unique<Timedep<TF>>(master, grid, "lw_flux_dn", swtimedep_prescribed);
    tdep_lw_flux_up = std::make_unique<Timedep<TF>>(master, grid, "lw_flux_up", swtimedep_prescribed);

    if (!swtimedep_prescribed)
    {
        // Get the surface fluxes, which are constant in time.
        sw_flux_dn = inputin.get_item<TF>("radiation", "sw_flux_dn", "");
        sw_flux_up = inputin.get_item<TF>("radiation", "sw_flux_up", "");
        lw_flux_dn = inputin.get_item<TF>("radiation", "lw_flux_dn", "");
        lw_flux_up = inputin.get_item<TF>("radiation", "lw_flux_up", "");
    }
}

template<typename TF>
void Radiation_prescribed<TF>::init(Timeloop<TF>& timeloop)
{
    auto& gd = grid.get_grid_data();

    // Resize surface radiation fields.
    sw_flux_dn_sfc.resize(gd.ijcells);
    sw_flux_up_sfc.resize(gd.ijcells);
    lw_flux_dn_sfc.resize(gd.ijcells);
    lw_flux_up_sfc.resize(gd.ijcells);
}

template<typename TF>
void Radiation_prescribed<TF>::create(
        Input& input, Netcdf_handle& input_nc, Thermo<TF>& thermo,
        Stats<TF>& stats, Column<TF>& column, Cross<TF>& cross, Dump<TF>& dump)
{
    if (swtimedep_prescribed)
    {
        std::string timedep_dim = "time_surface";

        tdep_sw_flux_dn->create_timedep(input_nc, timedep_dim);
        tdep_sw_flux_up->create_timedep(input_nc, timedep_dim);
        tdep_lw_flux_dn->create_timedep(input_nc, timedep_dim);
        tdep_lw_flux_up->create_timedep(input_nc, timedep_dim);

        // Add statistics
        const std::string group_name = "radiation";
        stats.add_time_series("sw_flux_dn_sfc", "Surface shortwave downwelling flux", "W m-2", group_name);
        stats.add_time_series("sw_flux_up_sfc", "Surface shortwave upwelling flux", "W m-2", group_name);
        stats.add_time_series("lw_flux_dn_sfc", "Surface longwave downwelling flux", "W m-2", group_name);
        stats.add_time_series("lw_flux_up_sfc", "Surface longwave upwelling flux", "W m-2", group_name);
    }
    else
    {
        // Set surface radiation fields for land-surface model.
        std::fill(sw_flux_dn_sfc.begin(), sw_flux_dn_sfc.end(), sw_flux_dn);
        std::fill(sw_flux_up_sfc.begin(), sw_flux_up_sfc.end(), sw_flux_up);
        std::fill(lw_flux_dn_sfc.begin(), lw_flux_dn_sfc.end(), lw_flux_dn);
        std::fill(lw_flux_up_sfc.begin(), lw_flux_up_sfc.end(), lw_flux_up);
    }
}

template<typename TF>
unsigned long Radiation_prescribed<TF>::get_time_limit(unsigned long itime)
{
    return Constants::ulhuge;
}

template <typename TF>
void Radiation_prescribed<TF>::update_time_dependent(Timeloop<TF>& timeloop)
{
    tdep_sw_flux_dn->update_time_dependent(sw_flux_dn, timeloop);
    tdep_sw_flux_up->update_time_dependent(sw_flux_up, timeloop);
    tdep_lw_flux_dn->update_time_dependent(lw_flux_dn, timeloop);
    tdep_lw_flux_up->update_time_dependent(lw_flux_up, timeloop);

    std::fill(sw_flux_dn_sfc.begin(), sw_flux_dn_sfc.end(), sw_flux_dn);
    std::fill(sw_flux_up_sfc.begin(), sw_flux_up_sfc.end(), sw_flux_up);
    std::fill(lw_flux_dn_sfc.begin(), lw_flux_dn_sfc.end(), lw_flux_dn);
    std::fill(lw_flux_up_sfc.begin(), lw_flux_up_sfc.end(), lw_flux_up);
}

template<typename TF>
void Radiation_prescribed<TF>::exec(
        Thermo<TF>& thermo, const double time, Timeloop<TF>& timeloop, Stats<TF>& stats)
{
}

template<typename TF>
void Radiation_prescribed<TF>::exec_all_stats(
        Stats<TF>& stats, Cross<TF>& cross,
        Dump<TF>& dump, Column<TF>& column,
        Thermo<TF>& thermo, Timeloop<TF>& timeloop,
        const unsigned long itime, const int iotime)
{
    if (stats.do_statistics(itime) && swtimedep_prescribed)
    {
        stats.set_time_series("sw_flux_dn_sfc", sw_flux_dn);
        stats.set_time_series("sw_flux_up_sfc", sw_flux_up);
        stats.set_time_series("lw_flux_dn_sfc", lw_flux_dn);
        stats.set_time_series("lw_flux_up_sfc", lw_flux_up);
    }
}

template<typename TF>
std::vector<TF>& Radiation_prescribed<TF>::get_surface_radiation(std::string name)
{
    if (name == "sw_down")
        return sw_flux_dn_sfc;
    else if (name == "sw_up")
        return sw_flux_up_sfc;
    else if (name == "lw_down")
        return lw_flux_dn_sfc;
    else if (name == "lw_up")
        return lw_flux_up_sfc;
    else
    {
        std::string error = "Variable \"" + name + "\" is not a valid surface radiation field";
        throw std::runtime_error(error);
    }
}

template class Radiation_prescribed<double>;
template class Radiation_prescribed<float>;
