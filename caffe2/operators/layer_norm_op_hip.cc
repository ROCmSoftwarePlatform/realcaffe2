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

#include "caffe2/operators/layer_norm_op.h"
#include "hip/hip_runtime.h"
#include <hipcub/hipcub.hpp>
#include "caffe2/core/context_hip.h"
#include "caffe2/utils/math.h"

namespace caffe2 {

namespace {
template <typename T>
struct SqrTransform
{
    inline __host__ __device__ T operator()(const T v) const { return v * v; }
};

// X = X - Y^2
__global__ void sqrtXMinusYSquaredKernel(const int N, float* x, const float* y, const float epsilon)
{
    HIP_1D_KERNEL_LOOP(i, N) { x[i] = sqrtf(x[i] - y[i] * y[i] + epsilon); }
}

// out[i, j] = (X[i, j] - mu[i]) / sigma[i]
__global__ void normalizeKernel(
    const int row_dim, const int N, const float* x, const float* mu, const float* sigma, float* out)
{
    HIP_1D_KERNEL_LOOP(i, N) { out[i] = (x[i] - mu[i / row_dim]) / (sigma[i / row_dim]); }
}

template <typename InputIterator_t>
void allocScratchAndReduce(InputIterator_t input,
                           float* output,
                           int num_segments,
                           int* seg_indices,
                           Tensor<HIPContext>* scratch,
                           hipStream_t stream)
{
    size_t temp_storage_bytes;
    hipcub::DeviceSegmentedReduce::Sum(nullptr, // To retrieve required temporary storage size
                                    temp_storage_bytes, // size_t &temp_storage_bytes
                                    input,              // InputIteratorT d_i
                                    output,             // OutputIteratorT d_out
                                    num_segments,       // int num_segments
                                    seg_indices,        // int *d_begin_offsets
                                    seg_indices + 1,    // int *d_end_offsets
                                    stream              // hipStream_t stream=0
                                    );
    size_t temp_storage_floats =
        temp_storage_bytes / sizeof(float) + (temp_storage_bytes % sizeof(float) ? 1 : 0);
    scratch->Resize(vector<size_t>{temp_storage_floats});

    hipcub::DeviceSegmentedReduce::Sum(
        scratch->mutable_data<float>(), // To retrieve required temporary storage
                                        // size
        temp_storage_bytes,             // size_t &temp_storage_bytes
        input,                          // InputIteratorT d_i
        output,                         // OutputIteratorT d_out
        num_segments,                   // int num_segments
        seg_indices,                    // int *d_begin_offsets
        seg_indices + 1,                // int *d_end_offsets
        stream                          // hipStream_t stream=0
        );
}

} //  namespace

template <>
template <>
bool LayerNormOp<HIPContext>::DoRunWithType<float>()
{
    const auto& input = Input(0);
    auto* output      = Output(0);
    auto* mean        = Output(1);
    auto* stdev       = Output(2);

    CAFFE_ENFORCE_GE(input.dims().size(), 2, "LayerNorm requires input dim >= 2");

    const auto canonical_axis = input.canonical_axis_index(axis_);
    const int left            = input.size_to_dim(canonical_axis);
    const int right           = input.size_from_dim(canonical_axis);

    output->ResizeLike(input);
    std::vector<TIndex> stats_dims(input.dims().begin(), input.dims().begin() + canonical_axis);
    stats_dims.push_back(1);
    mean->Resize(stats_dims);
    stdev->Resize(stats_dims);

    std::vector<int> segs(left + 1);
    std::iota(segs.begin(), segs.end(), 0);
    std::transform(
        segs.begin(), segs.end(), segs.begin(), std::bind1st(std::multiplies<int>(), right));

    seg_indices_.Resize(vector<size_t>{segs.size()});
    context_.CopyBytes<CPUContext, HIPContext>(
        sizeof(int) * segs.size(),
        static_cast<void*>(segs.data()),
        static_cast<void*>(seg_indices_.mutable_data<int>()));

    if(right == 1)
    {
        mean->CopyFrom(input);
        mean->Resize(stats_dims);
        math::Set<float, HIPContext>(
            left, std::sqrt(epsilon_), stdev->mutable_data<float>(), &context_);
    }
    else
    {
        // Calculate row-wise means
        // First stage: sum up feature vectors
        allocScratchAndReduce(input.data<float>(),
                              mean->mutable_data<float>(),
                              left,
                              seg_indices_.mutable_data<int>(),
                              &scratch_,
                              context_.hip_stream());

        // Second stage: Normalize by feature vector dim
        math::Scale<float, HIPContext>(left,
                                       1.0f / right,
                                       mean->mutable_data<float>(),
                                       mean->mutable_data<float>(),
                                       &context_);

        // Calculate row-wise standard deviation

        // First stage: sum up row-wise squared values
        SqrTransform<float> transform;
        hipcub::TransformInputIterator<float, SqrTransform<float>, const float*> it(
            input.data<float>(), transform);
        allocScratchAndReduce(it,
                              stdev->mutable_data<float>(),
                              left,
                              seg_indices_.mutable_data<int>(),
                              &scratch_,
                              context_.hip_stream());

        // Second stage: Normalize by feature vector dim
        math::Scale<float, HIPContext>(left,
                                       1.0f / right,
                                       stdev->mutable_data<float>(),
                                       stdev->mutable_data<float>(),
                                       &context_);

        // stddev = sqrt(E(x^2) - E(x)^2 + epsilon)
        hipLaunchKernelGGL((sqrtXMinusYSquaredKernel),
                           dim3(CAFFE_GET_BLOCKS(left)),
                           dim3(CAFFE_HIP_NUM_THREADS),
                           0,
                           context_.hip_stream(),
                           left,
                           stdev->mutable_data<float>(),
                           mean->mutable_data<float>(),
                           epsilon_);
    }

    // out[i, j] = (in[i,j] - mu[i]) / (sigma[i])
    hipLaunchKernelGGL((normalizeKernel),
                       dim3(CAFFE_GET_BLOCKS(left)),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context_.hip_stream(),
                       right,
                       left * right,
                       input.data<float>(),
                       mean->data<float>(),
                       stdev->data<float>(),
                       output->mutable_data<float>());

    return true;
}

REGISTER_HIP_OPERATOR(LayerNorm, LayerNormOp<HIPContext>);

namespace {
// x : [N, D]
// y : [N, 1]
// z : [N, D]
// (x - broadcast(y)) * z
__global__ void
zTimesXminusYbroadcast(int N, int D, const float* x, const float* y, const float* z, float* out)
{
    HIP_1D_KERNEL_LOOP(i, N * D) { out[i] = (x[i] - y[i / D]) * z[i]; }
}

__global__ void normalizeByNegStdev(int N, bool var, const float* x, const float* stdev, float* out)
{
    if(var)
    {
        HIP_1D_KERNEL_LOOP(i, N) { out[i] = (-1.0f * x[i]) / (stdev[i] * stdev[i]); }
    }
    else
    {
        HIP_1D_KERNEL_LOOP(i, N) { out[i] = (-1.0f * x[i]) / (stdev[i]); }
    }
}

__global__ void gradientMegaKernel(int N,
                                   int D,
                                   const float* stdev,
                                   const float* X,
                                   const float* dstdev,
                                   const float* dmean,
                                   const float* dout,
                                   float* out)
{
    HIP_1D_KERNEL_LOOP(i, N * D)
    {
        out[i] = 1.0f / stdev[i / D] * dout[i] + X[i] / (D * stdev[i / D]) * dstdev[i / D] +
                 1.0f / D * dmean[i / D];
    }
}

#define PRINT(X, N, D) \
    hipLaunchKernelGGL((printTensor), dim3(1), dim3(1), 0, context_.hip_stream(), X, N, D)

} // namespace

template <>
template <>
bool LayerNormGradientOp<HIPContext>::DoRunWithType<float>()
{
    const auto& dout         = Input(0);
    const auto& norm_outputs = Input(1);
    const auto& means        = Input(2);
    const auto& stdev        = Input(3);
    const auto& norm_inputs  = Input(4);
    auto* ginput             = Output(0);

    const auto canonical_axis = norm_inputs.canonical_axis_index(axis_);
    const unsigned long left  = norm_inputs.size_to_dim(canonical_axis);
    const unsigned long right = norm_inputs.size_from_dim(canonical_axis);

    ginput->ResizeLike(norm_inputs);
    std::vector<TIndex> stats_dims(norm_inputs.dims().begin(),
                                   norm_inputs.dims().begin() + canonical_axis);
    stats_dims.push_back(1);
    dmean_.Resize(stats_dims);
    dstdev_.Resize(stats_dims);
    gscratch_.Resize(std::vector<size_t>{left, right});

    std::vector<int> segs(left + 1);
    std::iota(segs.begin(), segs.end(), 0);
    std::transform(
        segs.begin(), segs.end(), segs.begin(), std::bind1st(std::multiplies<int>(), right));

    seg_indices_.Resize(vector<size_t>{segs.size()});
    context_.CopyBytes<CPUContext, HIPContext>(
        sizeof(int) * segs.size(),
        static_cast<void*>(segs.data()),
        static_cast<void*>(seg_indices_.mutable_data<int>()));

    // Calculate gradient of the standard deviation
    // temp1 = (x - mean) * dout
    hipLaunchKernelGGL((zTimesXminusYbroadcast),
                       dim3(CAFFE_GET_BLOCKS(left * right)),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context_.hip_stream(),
                       static_cast<int>(left),
                       static_cast<int>(right),
                       norm_inputs.data<float>(),
                       means.data<float>(),
                       dout.data<float>(),
                       gscratch_.mutable_data<float>());

    dstdev_.Resize(vector<size_t>{left, 1});
    // dstdev = reduce(temp1)
    allocScratchAndReduce(gscratch_.data<float>(),
                          dstdev_.mutable_data<float>(),
                          left,
                          seg_indices_.mutable_data<int>(),
                          &scratch_,
                          context_.hip_stream());
    // dstdev = -dstdev / sqrt(stdev)
    hipLaunchKernelGGL((normalizeByNegStdev),
                       dim3(CAFFE_GET_BLOCKS(left)),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context_.hip_stream(),
                       static_cast<int>(left),
                       true,
                       dstdev_.data<float>(),
                       stdev.data<float>(),
                       dstdev_.mutable_data<float>());

    // Calculate gradient of the mean
    // dmean = reduce(dout)
    allocScratchAndReduce(dout.data<float>(),
                          dmean_.mutable_data<float>(),
                          left,
                          seg_indices_.mutable_data<int>(),
                          &scratch_,
                          context_.hip_stream());
    // mean * stdev
    math::Mul(left,
              means.data<float>(),
              dstdev_.data<float>(),
              gscratch_.mutable_data<float>(),
              &context_);
    // [\sum dout] + mean * stdev
    math::Add(left,
              dmean_.data<float>(),
              gscratch_.data<float>(),
              dmean_.mutable_data<float>(),
              &context_);
    // -1 / std * [[\sum dout] + mean * stdev]
    hipLaunchKernelGGL((normalizeByNegStdev),
                       dim3(CAFFE_GET_BLOCKS(left)),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context_.hip_stream(),
                       static_cast<int>(left),
                       false,
                       dmean_.data<float>(),
                       stdev.data<float>(),
                       dmean_.mutable_data<float>());

    // Calculate gradient of input
    hipLaunchKernelGGL((gradientMegaKernel),
                       dim3(CAFFE_GET_BLOCKS(left)),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context_.hip_stream(),
                       static_cast<int>(left),
                       static_cast<int>(right),
                       stdev.data<float>(),
                       norm_inputs.data<float>(),
                       dstdev_.data<float>(),
                       dmean_.data<float>(),
                       dout.data<float>(),
                       ginput->mutable_data<float>());

    return true;
}

REGISTER_HIP_OPERATOR(LayerNormGradient, LayerNormGradientOp<HIPContext>);

} // namespace caffe2
