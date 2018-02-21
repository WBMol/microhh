/*
 * MicroHH
 * Copyright (c) 2011-2017 Chiel van Heerwaarden
 * Copyright (c) 2011-2017 Thijs Heus
 * Copyright (c) 2014-2017 Bart van Stratum
 *
 * The cuda_safe_call() and cuda_check_error() are from
 * http://choorucode.com/2011/03/02/how-to-do-error-checking-in-cuda/
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

#include <stdio.h>
#include "float.h"
#include "tools.h"

namespace Tools_g
{
    template <typename TF, ReduceType function> __device__
    TF reduction(TF v1, TF v2)
    {
        TF rval;
        if (function == sumType)
            rval = v1+v2;
        else if (function == maxType)
            rval = fmax(v1,v2);
        return rval;
    }

    // Reduce one block of data
    template <typename TF, ReduceType function, int blockSize> __device__
    void reduce_block_kernel(volatile TF* as, const unsigned int tid)
    {
        /* Loop is completely unrolled for performance */
        if (blockSize >= 512) { if (tid < 256) { as[tid] = reduction<TF, function>(as[tid],as[tid + 256]); } __syncthreads(); }
        if (blockSize >= 256) { if (tid < 128) { as[tid] = reduction<TF, function>(as[tid],as[tid + 128]); } __syncthreads(); }
        if (blockSize >= 128) { if (tid <  64) { as[tid] = reduction<TF, function>(as[tid],as[tid +  64]); } __syncthreads(); }

        /* Once we get to the last 32 values (1 thread warp), the __syncthreads() is no longer necessary */
        if (tid < 32)
        {
            if (blockSize >=  64) { if (tid < 32) { as[tid] = reduction<TF, function>(as[tid],as[tid + 32]); }}
            if (blockSize >=  32) { if (tid < 16) { as[tid] = reduction<TF, function>(as[tid],as[tid + 16]); }}
            if (blockSize >=  16) { if (tid <  8) { as[tid] = reduction<TF, function>(as[tid],as[tid +  8]); }}
            if (blockSize >=   8) { if (tid <  4) { as[tid] = reduction<TF, function>(as[tid],as[tid +  4]); }}
            if (blockSize >=   4) { if (tid <  2) { as[tid] = reduction<TF, function>(as[tid],as[tid +  2]); }}
            if (blockSize >=   2) { if (tid <  1) { as[tid] = reduction<TF, function>(as[tid],as[tid +  1]); }}
        }
    }

    // Reduce field from 3D to 2D, excluding ghost cells and padding
    template <typename TF, ReduceType function, int blockSize> __global__
    void reduce_interior_kernel(const TF* a, TF* a2d,
                        unsigned int istart, unsigned int jstart, unsigned int kstart,
                        unsigned int iend,   unsigned int jend,
                        unsigned int icells, unsigned int ijcells)
    {
        // See https://stackoverflow.com/a/27570775/3581217
        extern __shared__ __align__(sizeof(TF)) unsigned char as_tmp[];
        TF *as = reinterpret_cast<TF*>(as_tmp);

        const unsigned int tid  = threadIdx.x;
        const unsigned int i    = istart + threadIdx.x;
        const unsigned int j    = jstart + blockIdx.y;
        const unsigned int k    = kstart + blockIdx.z;
        const unsigned int jk   = blockIdx.y+blockIdx.z*(jend-jstart);   // Index in 2D "a2d"
        const unsigned int ijk  = i + j*icells + k*ijcells;              // Index in 3D "a"
        const unsigned int ijkm = iend + j*icells + k*ijcells;    // Max index in X-direction

        TF tmpval;
        if (function == maxType)
            tmpval = -DBL_MAX;
        else if (function == sumType)
            tmpval = 0;

        int ii = ijk;
        while (ii < ijkm)
        {
            tmpval = reduction<TF, function>(tmpval,a[ii]);
            if (ii + blockDim.x < ijkm)
                tmpval = reduction<TF, function>(tmpval,a[ii+blockDim.x]);
            ii += 2*blockDim.x;
        }
        as[tid] = tmpval;

        __syncthreads();

        reduce_block_kernel<TF, function, blockSize>(as, tid);

        if (tid == 0)
            a2d[jk] = as[0];
    }

    // Reduce array, not accounting from ghost cells or padding
    template <typename TF, ReduceType function, int blockSize> __global__
    void reduce_all_kernel(const TF* a, TF* aout, unsigned int ncells, unsigned int nvaluesperblock, TF scalefac)
    {
        // See https://stackoverflow.com/a/27570775/3581217
        extern __shared__ __align__(sizeof(TF)) unsigned char as_tmp[];
        TF *as = reinterpret_cast<TF*>(as_tmp);

        const unsigned int tid  = threadIdx.x;
        const unsigned int iim  = nvaluesperblock * (blockIdx.x+1);
        unsigned int ii         = nvaluesperblock *  blockIdx.x + threadIdx.x;

        TF tmpval;
        if (function == maxType)
            tmpval = -DBL_MAX;
        else if (function == sumType)
            tmpval = 0;

        while (ii < iim)
        {
            tmpval = reduction<TF, function>(tmpval,a[ii]);
            if (ii + blockDim.x < iim && ii + blockDim.x < ncells)
                tmpval = reduction<TF, function>(tmpval,a[ii+blockDim.x]);
            ii += 2*blockDim.x;
        }
        as[tid] = tmpval * scalefac;

        /* Make sure all threads are synchronised before reducing the shared array */
        __syncthreads();

        /* Reduce block in shared memory */
        reduce_block_kernel<TF, function, blockSize>(as, tid);

        /* First value in shared array now holds the reduced value. Write back to global memory */
        if (tid == 0)
            aout[blockIdx.x] = as[0];
    }

    int nextpow2(unsigned int x)
    {
        return (int)pow(2,ceil(log(x)/log(2)));
    }


    template<typename TF>
    void reduce_interior(TF* a, TF* a2d,
                         int itot, int istart, int iend,
                         int jtot, int jstart, int jend,
                         int ktot, int kstart,
                         int icells, int ijcells, ReduceType mode)
    {
        const int nthreads = max(16,min(reduceMaxThreads, nextpow2(itot/2)));

        dim3 gridGPU (1, jtot, ktot);
        dim3 blockGPU(nthreads, 1, 1);

        if (mode == maxType)
        {
            switch (nthreads)
            {
                case 512:
                    reduce_interior_kernel<TF, maxType, 512><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
                case 256:
                    reduce_interior_kernel<TF, maxType, 256><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
                case 128:
                    reduce_interior_kernel<TF, maxType, 128><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
                case 64:
                    reduce_interior_kernel<TF, maxType,  64><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
                case 32:
                    reduce_interior_kernel<TF, maxType,  32><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
                case 16:
                    reduce_interior_kernel<TF, maxType,  16><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
            }
        }
        else if (mode == sumType)
        {
            switch (nthreads)
            {
                case 512:
                    reduce_interior_kernel<TF, sumType, 512><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
                case 256:
                    reduce_interior_kernel<TF, sumType, 256><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
                case 128:
                    reduce_interior_kernel<TF, sumType, 128><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
                case 64:
                    reduce_interior_kernel<TF, sumType,  64><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
                case 32:
                    reduce_interior_kernel<TF, sumType,  32><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
                case 16:
                    reduce_interior_kernel<TF, sumType,  16><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, a2d, istart, jstart, kstart, iend, jend, icells, ijcells); break;
            }
        }
        cuda_check_error();
    }

    template<typename TF>
    void reduce_all(TF* a, TF* aout, int ncells, int nblocks, int nvaluesperblock, ReduceType mode, TF scalefac)
    {
       const int nthreads = max(16,min(reduceMaxThreads, nextpow2(nvaluesperblock/2)));

        dim3 gridGPU (nblocks,  1, 1);
        dim3 blockGPU(nthreads, 1, 1);

        if (mode == maxType)
        {
            switch (nthreads)
            {
                case 512:
                    reduce_all_kernel<TF, maxType, 512><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
                case 256:
                    reduce_all_kernel<TF, maxType, 256><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
                case 128:
                    reduce_all_kernel<TF, maxType, 128><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
                case 64:
                    reduce_all_kernel<TF, maxType,  64><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
                case 32:
                    reduce_all_kernel<TF, maxType,  32><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
                case 16:
                    reduce_all_kernel<TF, maxType,  16><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
            }
        }
        else if (mode == sumType)
        {
            switch (nthreads)
            {
                case 512:
                    reduce_all_kernel<TF, sumType, 512><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
                case 256:
                    reduce_all_kernel<TF, sumType, 256><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
                case 128:
                    reduce_all_kernel<TF, sumType, 128><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
                case 64:
                    reduce_all_kernel<TF, sumType,  64><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
                case 32:
                    reduce_all_kernel<TF, sumType,  32><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
                case 16:
                    reduce_all_kernel<TF, sumType,  16><<<gridGPU, blockGPU, nthreads*sizeof(TF)>>>(a, aout, ncells, nvaluesperblock, scalefac); break;
            }
        }
        cuda_check_error();
    }
}

template void Tools_g::reduce_interior<double>(double*, double*, int, int, int, int, int, int, int, int, int, int, Tools_g::ReduceType);
template void Tools_g::reduce_interior<float>(float*, float*, int, int, int, int, int, int, int, int, int, int, Tools_g::ReduceType);
template void Tools_g::reduce_all<double>(double*, double*, int, int, int, Tools_g::ReduceType, double);
template void Tools_g::reduce_all<float>(float*, float*, int, int, int, Tools_g::ReduceType, float);