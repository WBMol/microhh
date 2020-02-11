/*
 * MicroHH
 * Copyright (c) 2011-2017 Chiel van Heerwaarden
 * Copyright (c) 2011-2017 Thijs Heus
 * Copyright (c) 2014-2017 Bart van Stratum
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

#include <cstdio>
#include <iostream>
#include <cmath>
#include "master.h"
#include "grid.h"
#include "field3d.h"
#include "fields.h"
#include "defines.h"
#include "field3d_io.h"

template<typename TF>
Field3d_io<TF>::Field3d_io(Master& masterin, Grid<TF>& gridin) :
    master(masterin), grid(gridin),
    transpose(master, grid)
{
    sw_transpose = true;    // tmp here for now, becomes argument of save_field3d et al.
}

template<typename TF>
Field3d_io<TF>::~Field3d_io()
{
}

#ifdef USEMPI
namespace
{
    template<typename TF> MPI_Datatype mpi_fp_type();
    template<> MPI_Datatype mpi_fp_type<double>() { return MPI_DOUBLE; }
    template<> MPI_Datatype mpi_fp_type<float>() { return MPI_FLOAT; }
}

template<typename TF>
int Field3d_io<TF>::save_field3d(
        TF* restrict data, TF* restrict tmp1, TF* restrict tmp2,
        const char* filename, TF offset)
{
    // Save the data in transposed order to have large chunks of contiguous disk space.
    // MPI-IO is not stable on Juqueen and Supermuc otherwise
    auto& gd = grid.get_grid_data();
    auto& md = master.get_MPI_data();
    auto tp = Transpose<TF>(master, grid);
    tp.init();

    // extract the data from the 3d field without the ghost cells
    const int jj  = gd.icells;
    const int kk  = gd.icells*gd.jcells;
    const int jjb = gd.imax;
    const int kkb = gd.imax*gd.jmax;

    int count = gd.imax*gd.jmax*gd.kmax;

    MPI_Datatype subarray;   // MPI datatype containing the dimensions of the total array that is contained in one process.

    for (int k=0; k<gd.kmax; ++k)
        for (int j=0; j<gd.jmax; ++j)
            #pragma ivdep
            for (int i=0; i<gd.imax; ++i)
            {
                const int ijk  = i+gd.igc + (j+gd.jgc)*jj + (k+gd.kgc)*kk;
                const int ijkb = i + j*jjb + k*kkb;
                tmp1[ijkb] = data[ijk] + offset;
            }

    if (sw_transpose)
    {
        // Transpose the 3D field
        tp.exec_zx(tmp2, tmp1);

        // Create MPI datatype for writing transposed field
        int totsize [3] = {gd.kmax,   gd.jtot, gd.itot};
        int subsize [3] = {gd.kblock, gd.jmax, gd.itot};
        int substart[3] = {md.mpicoordx*gd.kblock, md.mpicoordy*gd.jmax, 0};
        MPI_Type_create_subarray(3, totsize, subsize, substart, MPI_ORDER_C, mpi_fp_type<TF>(), &subarray);
        MPI_Type_commit(&subarray);
    }
    else
    {
        // Create MPI datatype for writing non-transposed field
        int totsize [3] = {gd.kmax, gd.jtot, gd.itot};
        int subsize [3] = {gd.kmax, gd.jmax, gd.imax};
        int substart[3] = {0, md.mpicoordy*gd.jmax, md.mpicoordx*gd.imax};
        MPI_Type_create_subarray(3, totsize, subsize, substart, MPI_ORDER_C, mpi_fp_type<TF>(), &subarray);
        MPI_Type_commit(&subarray);
    }

    MPI_File fh;
    if (MPI_File_open(md.commxy, filename, MPI_MODE_CREATE | MPI_MODE_WRONLY | MPI_MODE_EXCL, MPI_INFO_NULL, &fh))
        return 1;

    // select noncontiguous part of 3d array to store the selected data
    MPI_Offset fileoff = 0; // the offset within the file (header size)
    char name[] = "native";

    if (MPI_File_set_view(fh, fileoff, mpi_fp_type<TF>(), subarray, name, MPI_INFO_NULL))
        return 1;

    if (sw_transpose)
    {
        if (MPI_File_write_all(fh, tmp2, count, mpi_fp_type<TF>(), MPI_STATUS_IGNORE))
            return 1;
    }
    else
    {
        if (MPI_File_write_all(fh, tmp1, count, mpi_fp_type<TF>(), MPI_STATUS_IGNORE))
            return 1;
    }

    if (MPI_File_close(&fh))
        return 1;

    MPI_Type_free(&subarray);

    return 0;
}

template<typename TF>
int Field3d_io<TF>::load_field3d(TF* const restrict data, TF* const restrict tmp1, TF* const restrict tmp2, const char* filename, TF offset)
{
    // Read the data (optionally) in transposed order to have large chunks of contiguous disk space.
    // MPI-IO is not stable on Juqueen and supermuc otherwise.
    auto& gd = grid.get_grid_data();
    auto& md = master.get_MPI_data();
    auto tp = Transpose<TF>(master, grid);
    tp.init();

    MPI_Datatype subarray;   // MPI datatype containing the dimensions of the total array that is contained in one process.

    if (sw_transpose)
    {
        // Create MPI datatype for reading transposed field
        int totsize [3] = {gd.kmax  , gd.jtot, gd.itot};
        int subsize [3] = {gd.kblock, gd.jmax, gd.itot};
        int substart[3] = {md.mpicoordx*gd.kblock, md.mpicoordy*gd.jmax, 0};
        MPI_Type_create_subarray(3, totsize, subsize, substart, MPI_ORDER_C, mpi_fp_type<TF>(), &subarray);
        MPI_Type_commit(&subarray);
    }
    else
    {
        // Create MPI datatype for reading non-transposed field
        int totsize [3] = {gd.kmax, gd.jtot, gd.itot};
        int subsize [3] = {gd.kmax, gd.jmax, gd.imax};
        int substart[3] = {0, md.mpicoordy*gd.jmax, md.mpicoordx*gd.imax};
        MPI_Type_create_subarray(3, totsize, subsize, substart, MPI_ORDER_C, mpi_fp_type<TF>(), &subarray);
        MPI_Type_commit(&subarray);
    }

    // Read the file
    MPI_File fh;
    if (MPI_File_open(md.commxy, filename, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh))
        return 1;

    // select noncontiguous part of 3d array to store the selected data
    MPI_Offset fileoff = 0; // the offset within the file (header size)
    char name[] = "native";
    MPI_File_set_view(fh, fileoff, mpi_fp_type<TF>(), subarray, name, MPI_INFO_NULL);

    // extract the data from the 3d field without the ghost cells
    int count = gd.imax*gd.jmax*gd.kmax;

    if (sw_transpose)
    {
        if (MPI_File_read_all(fh, tmp1, count, mpi_fp_type<TF>(), MPI_STATUS_IGNORE))
            return 1;
    }
    else
    {
        if (MPI_File_read_all(fh, tmp2, count, mpi_fp_type<TF>(), MPI_STATUS_IGNORE))
            return 1;
    }

    if (MPI_File_close(&fh))
        return 1;

    // Transpose the 3D field
    if (sw_transpose)
        tp.exec_xz(tmp2, tmp1);

    const int jj  = gd.icells;
    const int kk  = gd.icells*gd.jcells;
    const int jjb = gd.imax;
    const int kkb = gd.imax*gd.jmax;

    for (int k=0; k<gd.kmax; ++k)
        for (int j=0; j<gd.jmax; ++j)
            #pragma ivdep
            for (int i=0; i<gd.imax; ++i)
            {
                const int ijk  = i+gd.igc + (j+gd.jgc)*jj + (k+gd.kgc)*kk;
                const int ijkb = i + j*jjb + k*kkb;
                data[ijk] = tmp2[ijkb] - offset;
            }

    MPI_Type_free(&subarray);

    return 0;
}

template<typename TF>
int Field3d_io<TF>::save_xz_slice(TF* restrict data, TF* restrict tmp, const char* filename, int jslice)
{
//    auto& gd = grid.get_grid_data();
//    auto& md = master.get_MPI_data();
//
//    // extract the data from the 3d field without the ghost cells
//    int nerror = 0;
//
//    const int jj  = gd.icells;
//    const int kk  = gd.icells*gd.jcells;
//    const int kkb = gd.imax;
//
//    int count = gd.imax*gd.kmax;
//
//    for (int k=0; k<gd.kmax; k++)
//        #pragma ivdep
//        for (int i=0; i<gd.imax; i++)
//        {
//            // take the modulus of jslice and gd.jmax to have the right offset within proc
//            const int ijk  = i+gd.igc + ((jslice%gd.jmax)+gd.jgc)*jj + (k+gd.kgc)*kk;
//            const int ijkb = i + k*kkb;
//            tmp[ijkb] = data[ijk];
//        }
//
//    if (md.mpicoordy == jslice/gd.jmax)
//    {
//        MPI_File fh;
//        if (MPI_File_open(md.commx, filename, MPI_MODE_CREATE | MPI_MODE_WRONLY | MPI_MODE_EXCL, MPI_INFO_NULL, &fh))
//            ++nerror;
//
//        // select noncontiguous part of 3d array to store the selected data
//        MPI_Offset fileoff = 0; // the offset within the file (header size)
//        char name[] = "native";
//
//        if (!nerror)
//            if (MPI_File_set_view(fh, fileoff, mpi_fp_type<TF>(), subxzslice, name, MPI_INFO_NULL))
//                ++nerror;
//
//        // only write at the procs that contain the slice
//        if (!nerror)
//            if (MPI_File_write_all(fh, tmp, count, mpi_fp_type<TF>(), MPI_STATUS_IGNORE))
//                ++nerror;
//
//        if (!nerror)
//            MPI_File_sync(fh);
//
//        if (!nerror)
//            if (MPI_File_close(&fh))
//                ++nerror;
//    }
//
//    // Gather errors from other processes
//    master.sum(&nerror,1);
//
//    MPI_Barrier(md.commxy);
//
//    return nerror;
}

template<typename TF>
int Field3d_io<TF>::save_yz_slice(TF* restrict data, TF* restrict tmp, const char* filename, int islice)
{
//    auto& gd = grid.get_grid_data();
//    auto& md = master.get_MPI_data();
//
//    // extract the data from the 3d field without the ghost cells
//    int nerror = 0;
//
//    const int jj = gd.icells;
//    const int kk = gd.ijcells;
//
//    const int kkb = gd.jmax;
//
//    int count = gd.jmax*gd.kmax;
//
//    // Strip off the ghost cells
//    for (int k=0; k<gd.kmax; k++)
//        #pragma ivdep
//        for (int j=0; j<gd.jmax; j++)
//        {
//            // take the modulus of jslice and jmax to have the right offset within proc
//            const int ijk  = (islice%gd.imax)+gd.igc + (j+gd.jgc)*jj + (k+gd.kgc)*kk;
//            const int ijkb = j + k*kkb;
//            tmp[ijkb] = data[ijk];
//        }
//
//    if (md.mpicoordx == islice/gd.imax)
//    {
//        MPI_File fh;
//        if (MPI_File_open(md.commy, filename, MPI_MODE_CREATE | MPI_MODE_WRONLY | MPI_MODE_EXCL, MPI_INFO_NULL, &fh))
//            ++nerror;
//
//        // select noncontiguous part of 3d array to store the selected data
//        MPI_Offset fileoff = 0; // the offset within the file (header size)
//        char name[] = "native";
//
//        if (!nerror)
//            if (MPI_File_set_view(fh, fileoff, mpi_fp_type<TF>(), subyzslice, name, MPI_INFO_NULL))
//                ++nerror;
//
//        // only write at the procs that contain the slice
//        if (!nerror)
//            if (MPI_File_write_all(fh, tmp, count, mpi_fp_type<TF>(), MPI_STATUS_IGNORE))
//                ++nerror;
//
//        if (!nerror)
//            MPI_File_sync(fh);
//
//        if (!nerror)
//            if (MPI_File_close(&fh))
//                ++nerror;
//    }
//
//    // Gather errors from other processes
//    master.sum(&nerror,1);
//
//    MPI_Barrier(md.commxy);
//
//    return nerror;
}

template<typename TF>
int Field3d_io<TF>::save_xy_slice(TF* restrict data, TF* restrict tmp, const char* filename, int kslice)
{
//    auto& gd = grid.get_grid_data();
//    auto& md = master.get_MPI_data();
//
//    // extract the data from the 3d field without the ghost cells
//    const int jj  = gd.icells;
//    const int kk  = gd.icells*gd.jcells;
//    const int jjb = gd.imax;
//
//    // Subtract the ghost cells in case of a pure 2d plane that does not have ghost cells.
//    if (kslice == -1)
//        kslice = -gd.kgc;
//
//    int count = gd.imax*gd.jmax;
//
//    for (int j=0; j<gd.jmax; j++)
//        #pragma ivdep
//        for (int i=0; i<gd.imax; i++)
//        {
//            // take the modulus of jslice and jmax to have the right offset within proc
//            const int ijk  = i+gd.igc + (j+gd.jgc)*jj + (kslice+gd.kgc)*kk;
//            const int ijkb = i + j*jjb;
//            tmp[ijkb] = data[ijk];
//        }
//
//    MPI_File fh;
//    if (MPI_File_open(md.commxy, filename, MPI_MODE_CREATE | MPI_MODE_WRONLY | MPI_MODE_EXCL, MPI_INFO_NULL, &fh))
//        return 1;
//
//    // select noncontiguous part of 3d array to store the selected data
//    MPI_Offset fileoff = 0; // the offset within the file (header size)
//    char name[] = "native";
//
//    if (MPI_File_set_view(fh, fileoff, mpi_fp_type<TF>(), subxyslice, name, MPI_INFO_NULL))
//        return 1;
//
//    // only write at the procs that contain the slice
//    if (MPI_File_write_all(fh, tmp, count, mpi_fp_type<TF>(), MPI_STATUS_IGNORE))
//        return 1;
//
//    MPI_File_sync(fh);
//
//    if (MPI_File_close(&fh))
//        return 1;
//
//    MPI_Barrier(md.commxy);
//
//    return 0;
}

template<typename TF>
int Field3d_io<TF>::load_xy_slice(TF* restrict data, TF* restrict tmp, const char* filename, int kslice)
{
//    auto& gd = grid.get_grid_data();
//    auto& md = master.get_MPI_data();
//
//    // extract the data from the 3d field without the ghost cells
//    const int jj = gd.icells;
//    const int kk = gd.icells*gd.jcells;
//    const int jjb = gd.imax;
//
//    // Subtract the ghost cells in case of a pure 2d plane that does not have ghost cells.
//    if (kslice == -1)
//        kslice = -gd.kgc;
//
//    int count = gd.imax*gd.jmax;
//
//    MPI_File fh;
//    if (MPI_File_open(md.commxy, filename, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh))
//        return 1;
//
//    // select noncontiguous part of 3d array to store the selected data
//    MPI_Offset fileoff = 0; // the offset within the file (header size)
//    char name[] = "native";
//
//    if (MPI_File_set_view(fh, fileoff, mpi_fp_type<TF>(), subxyslice, name, MPI_INFO_NULL))
//        return 1;
//
//    // only write at the procs that contain the slice
//    if (MPI_File_read_all(fh, tmp, count, mpi_fp_type<TF>(), MPI_STATUS_IGNORE))
//        return 1;
//
//    if (MPI_File_close(&fh))
//        return 1;
//
//    MPI_Barrier(md.commxy);
//
//    for (int j=0; j<gd.jmax; j++)
//        #pragma ivdep
//        for (int i=0; i<gd.imax; i++)
//        {
//            // take the modulus of jslice and jmax to have the right offset within proc
//            const int ijk  = i+gd.igc + (j+gd.jgc)*jj + (kslice+gd.kgc)*kk;
//            const int ijkb = i + j*jjb;
//            data[ijk] = tmp[ijkb];
//        }
//
//    return 0;
}

#else
template<typename TF>
int Field3d_io<TF>::save_field3d(TF* restrict data, TF* restrict tmp1, TF* restrict tmp2,
        const char* filename, const TF offset)
{
    auto& gd = grid.get_grid_data();

    FILE *pFile;
    pFile = fopen(filename, "wbx");

    if (pFile == NULL)
        return 1;

    const int jj = gd.icells;
    const int kk = gd.icells*gd.jcells;

    // first, add the offset to the data
    for (int k=gd.kstart; k<gd.kend; ++k)
        for (int j=gd.jstart; j<gd.jend; ++j)
            for (int i=gd.istart; i<gd.iend; ++i)
            {
                const int ijk = i + j*jj + k*kk;
                tmp1[ijk] = data[ijk] + offset;
            }

    // second, save the data to disk
    for (int k=gd.kstart; k<gd.kend; ++k)
        for (int j=gd.jstart; j<gd.jend; ++j)
        {
            const int ijk = gd.istart + j*jj + k*kk;
            if( fwrite(&tmp1[ijk], sizeof(TF), gd.imax, pFile) != (unsigned)gd.imax)
                return 1;
        }

    fclose(pFile);

    return 0;
}

template<typename TF>
int Field3d_io<TF>::load_field3d(TF* restrict data, TF* restrict tmp1, TF* restrict tmp2,
        const char* filename, const TF offset)
{
    auto& gd = grid.get_grid_data();

    FILE *pFile;
    pFile = fopen(filename, "rb");

    if (pFile == NULL)
        return 1;

    const int jj = gd.icells;
    const int kk = gd.icells*gd.jcells;

    // first, load the data from disk
    for (int k=gd.kstart; k<gd.kend; k++)
        for (int j=gd.jstart; j<gd.jend; j++)
        {
            const int ijk = gd.istart + j*jj + k*kk;
            if( fread(&tmp1[ijk], sizeof(TF), gd.imax, pFile) != (unsigned)gd.imax )
                return 1;
        }

    fclose(pFile);

    // second, remove the offset
    for (int k=gd.kstart; k<gd.kend; k++)
        for (int j=gd.jstart; j<gd.jend; j++)
            for (int i=gd.istart; i<gd.iend; i++)
            {
                const int ijk = i + j*jj + k*kk;
                data[ijk] = tmp1[ijk] - offset;
            }

    return 0;
}

template<typename TF>
int Field3d_io<TF>::save_xz_slice(TF* restrict data, TF* restrict tmp, const char* filename, int jslice)
{
    auto& gd = grid.get_grid_data();

    // extract the data from the 3d field without the ghost cells
    const int jj  = gd.icells;
    const int kk  = gd.icells*gd.jcells;
    const int kkb = gd.imax;

    const int count = gd.imax*gd.kmax;

    for (int k=0; k<gd.kmax; k++)
#pragma ivdep
        for (int i=0; i<gd.imax; i++)
        {
            // take the modulus of jslice and gd.jmax to have the right offset within proc
            const int ijk  = i+gd.igc + (jslice+gd.jgc)*jj + (k+gd.kgc)*kk;
            const int ijkb = i + k*kkb;
            tmp[ijkb] = data[ijk];
        }

    FILE *pFile;
    pFile = fopen(filename, "wbx");
    if (pFile == NULL)
        return 1;

    fwrite(tmp, sizeof(TF), count, pFile);
    fclose(pFile);

    return 0;
}

template<typename TF>
int Field3d_io<TF>::save_yz_slice(TF* restrict data, TF* restrict tmp, const char* filename, int islice)
{
    auto& gd = grid.get_grid_data();

    // Extract the data from the 3d field without the ghost cells
    const int jj = gd.icells;
    const int kk = gd.ijcells;

    const int kkb = gd.jmax;

    int count = gd.jmax*gd.kmax;

    // Strip off the ghost cells
    for (int k=0; k<gd.kmax; k++)
        #pragma ivdep
        for (int j=0; j<gd.jmax; j++)
        {
            // take the modulus of jslice and jmax to have the right offset within proc
            const int ijk  = (islice%gd.imax)+gd.igc + (j+gd.jgc)*jj + (k+gd.kgc)*kk;
            const int ijkb = j + k*kkb;
            tmp[ijkb] = data[ijk];
        }

    FILE *pFile;
    pFile = fopen(filename, "wbx");
    if (pFile == NULL)
        return 1;

    fwrite(tmp, sizeof(TF), count, pFile);
    fclose(pFile);

    return 0;
}

template<typename TF>
int Field3d_io<TF>::save_xy_slice(TF* restrict data, TF* restrict tmp, const char* filename, int kslice)
{
    auto& gd = grid.get_grid_data();

    // extract the data from the 3d field without the ghost cells
    const int jj  = gd.icells;
    const int kk  = gd.icells*gd.jcells;
    const int jjb = gd.imax;

    const int count = gd.imax*gd.jmax;

    // Subtract the ghost cells in case of a pure 2d plane that does not have ghost cells.
    if (kslice == -1)
        kslice = -gd.kgc;

    for (int j=0; j<gd.jmax; j++)
#pragma ivdep
        for (int i=0; i<gd.imax; i++)
        {
            // take the modulus of jslice and jmax to have the right offset within proc
            const int ijk  = i+gd.igc + (j+gd.jgc)*jj + (kslice+gd.kgc)*kk;
            const int ijkb = i + j*jjb;
            tmp[ijkb] = data[ijk];
        }

    FILE *pFile;
    pFile = fopen(filename, "wbx");
    if (pFile == NULL)
        return 1;

    fwrite(tmp, sizeof(TF), count, pFile);
    fclose(pFile);

    return 0;
}

template<typename TF>
int Field3d_io<TF>::load_xy_slice(TF* restrict data, TF* restrict tmp, const char* filename, int kslice)
{
    auto& gd = grid.get_grid_data();

    const int count = gd.imax*gd.jmax;

    FILE *pFile;
    pFile = fopen(filename, "rb");
    if (pFile == NULL)
        return 1;

    fread(tmp, sizeof(TF), count, pFile);
    fclose(pFile);

    // Subtract the ghost cells in case of a pure 2d plane that does not have ghost cells.
    if (kslice == -1)
        kslice = -gd.kgc;

    // put the data back into a field with ghost cells
    const int jj  = gd.icells;
    const int kk  = gd.icells*gd.jcells;
    const int jjb = gd.imax;

    for (int j=0; j<gd.jmax; j++)
#pragma ivdep
        for (int i=0; i<gd.imax; i++)
        {
            const int ijk  = i+gd.igc + (j+gd.jgc)*jj + (kslice+gd.kgc)*kk;
            const int ijkb = i + j*jjb;
            data[ijk] = tmp[ijkb];
        }

    return 0;
}
#endif

template class Field3d_io<double>;
template class Field3d_io<float>;
