#include "hip/hip_runtime.h"
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

#include "sparse_to_dense_op.h"

#include "caffe2/core/common_hip.h"
#include "caffe2/core/context_hip.h"

namespace caffe2 {

template <typename TInd, typename TData>
__global__ void SparseToDenseKernel(
    size_t N, TIndex block_nitems, const TInd* indices, const TData* vals, TData* dst)
{
    HIP_1D_KERNEL_LOOP(i, N)
    {
        int idx     = indices[i / block_nitems];
        int dst_idx = block_nitems * idx + i % block_nitems;
        atomicAdd(&dst[dst_idx], vals[i]);
    }
}

template <>
bool SparseToDenseOp<HIPContext>::RunOnDevice()
{
    return DispatchHelper<TensorTypes<int32_t>>::call(this, Input(INDICES));
}

template <>
template <typename TInd>
bool SparseToDenseOp<HIPContext>::DoRunWithType()
{
    return DispatchHelper<TensorTypes2<float, int32_t>, TInd>::call(this, Input(VALUES));
}

template <>
template <typename TInd, typename TData>
bool SparseToDenseOp<HIPContext>::DoRunWithType2()
{
    auto& sparse_indices = Input(INDICES);
    CAFFE_ENFORCE_EQ(sparse_indices.ndim(), 1);
    auto& sparse_values = Input(VALUES);
    CAFFE_ENFORCE_GE(sparse_values.ndim(), 1);
    CAFFE_ENFORCE_EQ(sparse_indices.size(), sparse_values.dim(0));

    const TInd* sparse_indices_vec   = sparse_indices.template data<TInd>();
    const int32_t sparse_indices_len = sparse_indices.dim32(0);
    const int output_first_dim       = GetOutputFirstDim(sparse_indices_vec, sparse_indices_len);

    auto shape   = sparse_values.dims();
    shape[0]     = output_first_dim;
    auto* output = Output(0);
    output->Resize(shape);

    TData* output_data = output->template mutable_data<TData>();
    math::Set<TData>(output->size(), TData(0), output_data, &context_);

    const auto block_nitems        = sparse_values.size_from_dim(1);
    const TData* sparse_values_vec = sparse_values.template data<TData>();

    size_t N = block_nitems * sparse_indices_len;
    CAFFE_ENFORCE_EQ(output->size(), output_first_dim * block_nitems);
    hipLaunchKernelGGL((SparseToDenseKernel<TInd, TData>),
                       dim3(CAFFE_GET_BLOCKS(N)),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context_.hip_stream(),
                       N,
                       block_nitems,
                       sparse_indices_vec,
                       sparse_values_vec,
                       output_data);

    return true;
}

REGISTER_HIP_OPERATOR(SparseToDense, SparseToDenseOp<HIPContext>);

} // namespace caffe2
