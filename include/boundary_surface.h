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

#ifndef BOUNDARY_SURFACE_H
#define BOUNDARY_SURFACE_H

#include "boundary.h"
#include "stats.h"

template<typename> class Diff;

template<typename TF>
class Boundary_surface : public Boundary<TF>
{
    public:
        Boundary_surface(Master&, Grid<TF>&, Fields<TF>&, Input&);
        ~Boundary_surface();

        void init(Input&, Thermo<TF>&);
        void create(Input&, Netcdf_handle&, Stats<TF>&, Column<TF>&, Cross<TF>&);
        void set_values();

        void get_ra(Field3d<TF>&);
        const std::vector<TF>& get_z0m() const { return z0m; };
        const std::vector<TF>& get_z0h() const { return z0h; };
        const std::vector<TF>& get_ustar() const { return ustar; };
        const std::vector<TF>& get_obuk() const { return obuk; };
        void get_duvdz(std::vector<TF>&, std::vector<TF>&);

        void calc_mo_stability(Thermo<TF>&);
        void calc_mo_bcs_momentum(Thermo<TF>&);
        void calc_mo_bcs_scalars(Thermo<TF>&);

        void exec_stats(Stats<TF>&);
        void exec_column(Column<TF>&);
        void exec_cross(Cross<TF>&, unsigned long);

        void load(const int);
        void save(const int);

        #ifdef USECUDA
        // GPU functions and variables
        void prepare_device();
        void clear_device();
        void forward_device();  // TMP BVS
        void backward_device(); // TMP BVS

        TF* get_z0m_g() { return z0m_g; };
        TF* get_ustar_g() { return ustar_g; };
        TF* get_obuk_g() { return obuk_g; };
        #endif

    protected:
        void process_input(Input&, Thermo<TF>&); // Process and check the surface input
        void init_surface(Input&); // Allocate and initialize the surface arrays
        void init_solver(); // Prepare the lookup table's for the surface layer solver
        void set_ustar(); // Set fixed ustar

    private:
        using Boundary<TF>::master;
        using Boundary<TF>::grid;
        using Boundary<TF>::fields;
        using Boundary<TF>::boundary_cyclic;
        using Boundary<TF>::swboundary;
        using Boundary<TF>::field3d_io;

        using Boundary<TF>::process_bcs;

        using Boundary<TF>::mbcbot;
        using Boundary<TF>::ubot;
        using Boundary<TF>::vbot;

        typedef std::map<std::string, Field3dBc<TF>> BcMap;
        using Boundary<TF>::sbc;

        TF ustarin;

        std::vector<float> zL_sl;
        std::vector<float> f_sl;

        std::vector<TF> z0m;
        std::vector<TF> z0h;

        std::vector<TF> ustar;
        std::vector<TF> obuk;
        std::vector<int> nobuk;

        #ifdef USECUDA
        TF* z0m_g;
        TF* z0h_g;
        TF* obuk_g;
        TF* ustar_g;
        int* nobuk_g;

        float* zL_sl_g;
        float* f_sl_g;
        #endif

        Boundary_type thermobc;
        bool sw_constant_z0;

    protected:
        // Cross sections
        std::vector<std::string> cross_list;         // List of active cross variables

        void update_slave_bcs();
};
#endif
