// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCRAND_RNG_MRG32K3A_H_
#define ROCRAND_RNG_MRG32K3A_H_

#include <algorithm>
#include <hip/hip_runtime.h>

#include <rocrand.h>

#include "generator_type.hpp"
#include "device_engines.hpp"
#include "distributions.hpp"

namespace rocrand_host {
namespace detail {

    typedef ::rocrand_device::mrg32k3a_engine mrg32k3a_device_engine;

    __global__
    void init_engines_kernel(mrg32k3a_device_engine * engines,
                            unsigned long long seed,
                            unsigned long long offset)
    {
        const unsigned int engine_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
        engines[engine_id] = mrg32k3a_device_engine(seed, engine_id, offset);
    }

    template<class Type, class Distribution>
    __global__
    void generate_kernel(mrg32k3a_device_engine * engines,
                         Type * data, const size_t n,
                         const Distribution distribution)
    {
        const unsigned int engine_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
        unsigned int index = engine_id;
        unsigned int stride = hipGridDim_x * hipBlockDim_x;

        // Load device engine
        mrg32k3a_device_engine engine = engines[engine_id];

        while(index < n)
        {
            data[index] = distribution(engine());
            // Next position
            index += stride;
        }

        // Save engine with its state
        engines[engine_id] = engine;
    }

    template<class RealType, class Distribution>
    __global__
    void generate_normal_kernel(mrg32k3a_device_engine * engines,
                                RealType * data, const size_t n,
                                Distribution distribution)
    {
        typedef decltype(distribution(engines->next(), engines->next())) RealType2;

        const unsigned int engine_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
        unsigned int index = engine_id;
        unsigned int stride = hipGridDim_x * hipBlockDim_x;

        // Load device engine
        mrg32k3a_device_engine engine = engines[engine_id];

        RealType2 * data2 = (RealType2 *)data;
        while(index < (n / 2))
        {
            data2[index] = distribution(engine(), engine());
            // Next position
            index += stride;
        }

        // First work-item saves the tail when n is not a multiple of 2
        if(engine_id == 0 && (n & 1) > 0)
        {
            RealType2 result = distribution(engine(), engine());
            // Save the tail
            data[n - 1] = result.x;
        }

        // Save engine with its state
        engines[engine_id] = engine;
    }

} // end namespace detail
} // end namespace rocrand_host

class rocrand_mrg32k3a : public rocrand_generator_type<ROCRAND_RNG_PSEUDO_MRG32K3A>
{
public:
    using base_type = rocrand_generator_type<ROCRAND_RNG_PSEUDO_MRG32K3A>;
    using engine_type = ::rocrand_host::detail::mrg32k3a_device_engine;

    rocrand_mrg32k3a(unsigned long long seed = 12345,
                     unsigned long long offset = 0,
                     hipStream_t stream = 0)
        : base_type(seed, offset, stream),
          m_engines_initialized(false), m_engines(NULL), m_engines_size(1024 * 256)
    {
        // Allocate device random number engines
        auto error = hipMalloc(&m_engines, sizeof(engine_type) * m_engines_size);
        if(error != hipSuccess)
        {
            throw ROCRAND_STATUS_ALLOCATION_FAILED;
        }
        if(m_seed == 0)
        {
            m_seed = ROCRAND_MRG32K3A_DEFAULT_SEED;
        }
    }

    ~rocrand_mrg32k3a()
    {
        hipFree(m_engines);
    }

    void reset()
    {
        m_engines_initialized = false;
    }

    /// Changes seed to \p seed and resets generator state.
    ///
    /// New seed value should not be zero. If \p seed_value is equal
    /// zero, value \p ROCRAND_MRG32K3A_DEFAULT_SEED is used instead.
    void set_seed(unsigned long long seed)
    {
        if(seed == 0)
        {
            seed = ROCRAND_MRG32K3A_DEFAULT_SEED;
        }
        m_seed = seed;
        m_engines_initialized = false;
    }

    void set_offset(unsigned long long offset)
    {
        m_offset = offset;
        m_engines_initialized = false;
    }

    rocrand_status init()
    {
        if (m_engines_initialized)
            return ROCRAND_STATUS_SUCCESS;

        #ifdef __HIP_PLATFORM_NVCC__
        const uint32_t threads = 128;
        const uint32_t max_blocks = 128;
        #else
        const uint32_t threads = 256;
        const uint32_t max_blocks = 1024;
        #endif
        const uint32_t blocks = max_blocks;

        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(rocrand_host::detail::init_engines_kernel),
            dim3(blocks), dim3(threads), 0, m_stream,
            m_engines, m_seed, m_offset
        );
        // Check kernel status
        if(hipPeekAtLastError() != hipSuccess)
            return ROCRAND_STATUS_LAUNCH_FAILURE;

        m_engines_initialized = true;

        return ROCRAND_STATUS_SUCCESS;
    }

    template<class T, class Distribution = mrg_uniform_distribution<T> >
    rocrand_status generate(T * data, size_t data_size,
                            const Distribution& distribution = Distribution())
    {
        rocrand_status status = init();
        if (status != ROCRAND_STATUS_SUCCESS)
            return status;

        #ifdef __HIP_PLATFORM_NVCC__
        const uint32_t threads = 128;
        const uint32_t max_blocks = 128; // 512
        #else
        const uint32_t threads = 256;
        const uint32_t max_blocks = 1024;
        #endif
        const uint32_t blocks = max_blocks;

        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(rocrand_host::detail::generate_kernel),
            dim3(blocks), dim3(threads), 0, m_stream,
            m_engines, data, data_size, distribution
        );
        // Check kernel status
        if(hipPeekAtLastError() != hipSuccess)
            return ROCRAND_STATUS_LAUNCH_FAILURE;

        return ROCRAND_STATUS_SUCCESS;
    }

    template<class T>
    rocrand_status generate_uniform(T * data, size_t data_size)
    {
        mrg_uniform_distribution<T> udistribution;
        return generate(data, data_size, udistribution);
    }

    template<class T>
    rocrand_status generate_normal(T * data, size_t data_size, T stddev, T mean)
    {
        rocrand_status status = init();
        if (status != ROCRAND_STATUS_SUCCESS)
            return status;

        #ifdef __HIP_PLATFORM_NVCC__
        const uint32_t threads = 128;
        const uint32_t max_blocks = 128; // 512
        #else
        const uint32_t threads = 256;
        const uint32_t max_blocks = 1024;
        #endif
        const uint32_t blocks = max_blocks;

        mrg_normal_distribution<T> distribution(mean, stddev);

        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(rocrand_host::detail::generate_normal_kernel),
            dim3(blocks), dim3(threads), 0, m_stream,
            m_engines, data, data_size, distribution
        );
        // Check kernel status
        if(hipPeekAtLastError() != hipSuccess)
            return ROCRAND_STATUS_LAUNCH_FAILURE;

        return ROCRAND_STATUS_SUCCESS;
    }

    template<class T>
    rocrand_status generate_log_normal(T * data, size_t data_size, T stddev, T mean)
    {
        rocrand_status status = init();
        if (status != ROCRAND_STATUS_SUCCESS)
            return status;

        #ifdef __HIP_PLATFORM_NVCC__
        const uint32_t threads = 128;
        const uint32_t max_blocks = 128; // 512
        #else
        const uint32_t threads = 256;
        const uint32_t max_blocks = 1024;
        #endif
        const uint32_t blocks = max_blocks;

        mrg_log_normal_distribution<T> distribution(mean, stddev);

        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(rocrand_host::detail::generate_normal_kernel),
            dim3(blocks), dim3(threads), 0, m_stream,
            m_engines, data, data_size, distribution
        );
        // Check kernel status
        if(hipPeekAtLastError() != hipSuccess)
            return ROCRAND_STATUS_LAUNCH_FAILURE;

        return ROCRAND_STATUS_SUCCESS;
    }

    rocrand_status generate_poisson(unsigned int * data, size_t data_size, double lambda)
    {
        try
        {
            poisson.set_lambda(lambda);
        }
        catch(rocrand_status status)
        {
            return status;
        }
        return generate(data, data_size, poisson.dis);
    }

private:
    bool m_engines_initialized;
    engine_type * m_engines;
    size_t m_engines_size;

    // For caching of Poisson for consecutive generations with the same lambda
    poisson_distribution_manager<> poisson;

    // m_seed from base_type
    // m_offset from base_type
};

#endif // ROCRAND_RNG_MRG32K3A_H_
