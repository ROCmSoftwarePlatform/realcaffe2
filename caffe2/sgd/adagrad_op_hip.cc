/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <hipcub/hipcub.hpp>
#include "adagrad_op.h"
#include "caffe2/core/common_hip.h"
#include "caffe2/core/context_hip.h"
#include "caffe2/utils/mixed_utils_hip.h"
#include "hip/hip_runtime.h"

namespace caffe2 {

__global__ void AdagradUpdate(int N,
                              const float* w,
                              const float* g,
                              const float* h,
                              float* nw,
                              float* nh,
                              float epsilon,
                              float decay,
                              const float* lr)
{
    HIP_1D_KERNEL_LOOP(i, N)
    {
        float gi = g[i];
        float hi = nh[i] = decay * h[i] + gi * gi;
        nw[i]            = w[i] + lr[0] * gi / (sqrtf(hi) + epsilon);
    }
}

template <>
void adagrad_update<HIPContext>(int N,
                                const float* w,
                                const float* g,
                                const float* h,
                                float* nw,
                                float* nh,
                                float epsilon,
                                float decay,
                                const float* lr,
                                HIPContext* context)
{
    hipLaunchKernelGGL((AdagradUpdate),
                       dim3(CAFFE_GET_BLOCKS(N)),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context->hip_stream(),
                       N,
                       w,
                       g,
                       h,
                       nw,
                       nh,
                       epsilon,
                       decay,
                       lr);
}

template <typename SIndex, typename THalf>
__global__ void SparseAdagradKernel(const size_t N,
                                    const size_t grad_slice_sz,
                                    const float epsilon,
                                    THalf* param,
                                    THalf* param_mom,
                                    const SIndex* indices,
                                    const float* grad,
                                    const float* lr)
{
    const float LR = lr[0];
    HIP_1D_KERNEL_LOOP(i, N)
    {
        const size_t gradIdx  = i;
        const SIndex index    = indices[i / grad_slice_sz];
        const size_t paramIdx = index * grad_slice_sz + (i % grad_slice_sz);

        float mom_new = mixed_add(grad[gradIdx] * grad[gradIdx], param_mom[paramIdx]);
        mixed_store(&mom_new, &(param_mom[paramIdx]));
        float param_new =
            mixed_add(LR * grad[gradIdx] / (sqrtf(mom_new) + epsilon), param[paramIdx]);
        mixed_store(&param_new, &(param[paramIdx]));
    }
}

/**
 * Calculate RowwiseSparseAdagrad
 * M: gradients.dims[0]
 * N: gradients.size_from_dim(1)
 * grad: pointer to the gradients
 * param: pointer to weights
 * param_mom: pointer to the momentum
 * indices: keys
 */
template <typename SIndex>
__global__ void RowWiseSparseAdagradKernel(const int M,
                                           const int N,
                                           const float epsilon,
                                           float* param,
                                           float* param_mom,
                                           const SIndex* indices,
                                           const float* grad,
                                           const float* lr)
{
    using BlockReduce = hipcub::BlockReduce<float, CAFFE_HIP_NUM_THREADS>;
    __shared__ BlockReduce::TempStorage temp_storage;
    // in case gridDim is smaller than M
    for(int i = hipBlockIdx_x; i < M; i += hipGridDim_x)
    {
        const SIndex index = indices[i];
        float sum_squares  = 0.0;
        __shared__ float row_sum_squares_avg;

        // in case N is bigger than block size which is 512 by default
        for(int j = hipThreadIdx_x; j < N; j += hipBlockDim_x)
        {
            const float x_ij = grad[i * N + j];
            sum_squares += x_ij * x_ij;
        }
        float reduce_result = BlockReduce(temp_storage).Sum(sum_squares);
        if(hipThreadIdx_x == 0)
        {
            row_sum_squares_avg = reduce_result / (float)N;
            param_mom[index] += row_sum_squares_avg;
        }
        __syncthreads();
        // update param
        float step = lr[0] / (sqrtf(param_mom[index]) + epsilon);
        for(int j = hipThreadIdx_x; j < N; j += hipBlockDim_x)
        {
            param[index * N + j] = param[index * N + j] + grad[i * N + j] * step;
        }
    }
}

template <typename T, class Context>
class HIPSparseAdagradOp final : public Operator<Context>
{
    public:
    USE_OPERATOR_CONTEXT_FUNCTIONS;
    HIPSparseAdagradOp(const OperatorDef& operator_def, Workspace* ws)
        : Operator<Context>(operator_def, ws),
          epsilon_(OperatorBase::GetSingleArgument<float>("epsilon", 1e-5f))
    {
        const T decay = OperatorBase::GetSingleArgument<T>("decay", 1.0f);
        CAFFE_ENFORCE_EQ(decay, 1.0, "Decay is not supported for SparseAdagradOp");
    }

    bool RunOnDevice() override
    {
        // Enforce shapes
        CAFFE_ENFORCE_EQ(Input(PARAM).size(), Input(MOMENT_1).size());
        CAFFE_ENFORCE_EQ(Input(LR).size(), 1);
        CAFFE_ENFORCE_EQ(Input(PARAM).size_from_dim(1),
                         Input(GRAD).size_from_dim(Input(INDICES).ndim()));

        return DispatchHelper<TensorTypes<int32_t, int64_t>>::call(this, Input(INDICES));
    }

    template <typename IndexType>
    bool DoRunWithType()
    {
        auto n = Input(INDICES).size();
        if(n == 0)
        {
            return true;
        }
        return DispatchHelper<TensorTypes2<float, float16>, IndexType>::call(this, Input(PARAM));
    }

    template <typename IndexType, typename THalf>
    bool DoRunWithType2()
    {
        const auto* lr       = Input(LR).template data<T>();
        const auto* indices  = Input(INDICES).template data<IndexType>();
        const auto* gradIn   = Input(GRAD).template data<T>();
        const auto* paramIn  = Input(PARAM).template data<THalf>();
        const auto* momentIn = Input(MOMENT_1).template data<THalf>();
        auto* paramOut       = Output(OUTPUT_PARAM)->template mutable_data<THalf>();
        auto* momentOut      = Output(OUTPUT_MOMENT_1)->template mutable_data<THalf>();

        auto N             = Input(GRAD).size();
        auto grad_slice_sz = Input(GRAD).size_from_dim(Input(INDICES).ndim());
        if(N == 0)
        {
            // empty grad, nothing to do here, not even launching the kernel
            return true;
        }
        hipLaunchKernelGGL((SparseAdagradKernel<IndexType, THalf>),
                           dim3(CAFFE_GET_BLOCKS(N)),
                           dim3(CAFFE_HIP_NUM_THREADS),
                           0,
                           context_.hip_stream(),
                           static_cast<const size_t>(N),
                           static_cast<const size_t>(grad_slice_sz),
                           static_cast<const float>(epsilon_),
                           Output(OUTPUT_PARAM)->template mutable_data<THalf>(),
                           Output(OUTPUT_MOMENT_1)->template mutable_data<THalf>(),
                           Input(INDICES).template data<IndexType>(),
                           Input(GRAD).template data<float>(),
                           Input(LR).template data<float>());
        return true;
    }

    protected:
    T epsilon_;
    INPUT_TAGS(PARAM, MOMENT_1, INDICES, GRAD, LR);
    OUTPUT_TAGS(OUTPUT_PARAM, OUTPUT_MOMENT_1);
};

template <>
template <typename SIndex>
bool RowWiseSparseAdagradOp<float, HIPContext>::DoRunWithType()
{
    auto N = Input(GRAD).size();
    if(N == 0)
    {
        // empty grad, nothing to do here, not even launching the kernel
        return true;
    }
    // size of the 1st dimension of the input gradient
    auto GRAD_M = Input(GRAD).dim32(0);
    auto GRAD_N = N / GRAD_M;

    // each thread block will handle multiple rows of the input and output
    hipLaunchKernelGGL((RowWiseSparseAdagradKernel),
                       dim3(min(GRAD_M, CAFFE_MAXIMUM_NUM_BLOCKS)),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context_.hip_stream(),
                       static_cast<const int>(GRAD_M),
                       static_cast<const int>(GRAD_N),
                       static_cast<const float>(epsilon_),
                       Output(OUTPUT_PARAM)->template mutable_data<float>(),
                       Output(OUTPUT_MOMENT_1)->template mutable_data<float>(),
                       Input(INDICES).template data<SIndex>(),
                       Input(GRAD).template data<float>(),
                       Input(LR).template data<float>());
    return true;
}

REGISTER_HIP_OPERATOR(Adagrad, AdagradOp<float, HIPContext>);
REGISTER_HIP_OPERATOR(SparseAdagrad, HIPSparseAdagradOp<float, HIPContext>);
REGISTER_HIP_OPERATOR(RowWiseSparseAdagrad, RowWiseSparseAdagradOp<float, HIPContext>);
}
