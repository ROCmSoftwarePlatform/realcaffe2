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

#include <cub/block/block_reduce.cuh>
#include "hip/hip_runtime.h"
#include "caffe2/core/context_hip.h"
#include "caffe2/operators/rmac_regions_op.h"

namespace cub {

template <typename KeyT, typename ValueT>
inline __host__ __device__ bool operator<(
    const cub::KeyValuePair<KeyT, ValueT>& kv1,
    const cub::KeyValuePair<KeyT, ValueT>& kv2) {
  return (kv1.value < kv2.value) ||
      (kv1.value == kv2.value && kv2.key < kv1.key);
}

} // namespace cub

namespace caffe2 {

namespace {

__global__ void NumRMACRegionsKernel(
    const int W,
    const int H,
    const int min_step,
    const int max_step,
    const float overlap,
    const int scales,
    int* num_rois_data) {
  // steps(idx) regions for long dimension
  typedef cub::KeyValuePair<int, float> KeyValuePair; // <step, value>
  KeyValuePair kv, min_kv;
  min_kv.value = FLT_MAX;

  // Local reduction
  int minW = min(H, W);
  int diff = max(H, W) - minW;
  HIP_1D_KERNEL_LOOP(index, max_step - min_step + 1) {
    kv.key = min_step + index;
    float b = diff / (1.0 * kv.key);
    kv.value = fabsf((minW * minW - minW * b) / (minW * minW) - overlap);

    if (kv < min_kv) {
      min_kv = kv;
    }
  }

  // Block-wise arg-min reduction to find step
  int step;
  {
    typedef cub::BlockReduce<KeyValuePair, CAFFE_HIP_NUM_THREADS> BlockReduce;
    __shared__ typename BlockReduce::TempStorage temp_storage;
    min_kv = BlockReduce(temp_storage).Reduce(min_kv, cub::Min());

    __shared__ int step_shared;
    if (hipThreadIdx_x == 0) {
      step_shared = min_kv.key;
    }
    __syncthreads();
    step = step_shared;
  }

  // Region overplus per dimension
  int Wd = (W > H) ? step : 0;
  int Hd = (H > W) ? step : 0;

  // Local reduction to compute the total number of rois at all scales
  int num_rois = 0;
  HIP_1D_KERNEL_LOOP(index, scales) {
    int l = index + 1;
    int region_size = 2 * minW / (l + 1);
    num_rois += (region_size > 0) ? ((l + Wd) * (l + Hd)) : 0;
  }

  // Block-wise sum reduction to compute num_rois at all scales
  {
    typedef cub::BlockReduce<int, CAFFE_HIP_NUM_THREADS> BlockReduce;
    __shared__ typename BlockReduce::TempStorage temp_storage;
    num_rois = BlockReduce(temp_storage).Sum(num_rois);
  }

  if (hipThreadIdx_x == 0) {
    num_rois_data[0] = num_rois;
    num_rois_data[1] = Wd;
    num_rois_data[2] = Hd;
  }
}

__global__ void RMACRegionsKernel(
    const int W,
    const int H,
    const int N,
    const int* num_rois_data,
    float* output) {
  int num_rois = num_rois_data[0];
  int Wd = num_rois_data[1];
  int Hd = num_rois_data[2];

  // Block-wide temp shared storage for intermediate ROI results to avoid
  // uncoalesced writes to global mem
  __shared__ float output_shared[CAFFE_HIP_NUM_THREADS * 5];

  HIP_1D_KERNEL_LOOP(index, N) {
    int batch_id = index / num_rois;
    int roi_id = index % num_rois;

    int roi[5];
    roi[0] = batch_id;

    // Find the scale corresponding to this index and the roi_id relative
    // to the scale.
    int l = 0;
    int num_rois_at_scale = 0;
    do {
      roi_id -= num_rois_at_scale;
      l++;
      num_rois_at_scale = (l + Wd) * (l + Hd);
    } while (roi_id - num_rois_at_scale >= 0);

    int region_size = 2 * min(H, W) / (l + 1);
    float bw =
        (l + Wd - 1 > 0) ? ((W - region_size) / (1.0 * (l + Wd - 1))) : 0;
    float bh =
        (l + Hd - 1 > 0) ? ((H - region_size) / (1.0 * (l + Hd - 1))) : 0;

    int i = roi_id / (l + Hd);
    int j = roi_id % (l + Hd);

    roi[1] = bw * i;
    roi[2] = bh * j;
    // Careful with the borders
    if (roi[1] + region_size > W) {
      roi[1] -= (roi[1] + region_size - W);
    }
    if (roi[2] + region_size > H) {
      roi[2] -= (roi[2] + region_size - H);
    }
    roi[3] = roi[1] + region_size - 1;
    roi[4] = roi[2] + region_size - 1;

    // Writing directly to output (global memory) will result in uncoalesced
    // writes. Write output to shared mem first and then write ROI results to
    // global output in a coalesced manner.
    __syncthreads(); // Since output_shared is reused across loop iterations
    for (int i = 0; i < 5; ++i) {
      output_shared[hipThreadIdx_x * 5 + i] = roi[i];
    }
    __syncthreads();
    int offset = index - hipThreadIdx_x;
    float* output_offset = output + offset * 5;
    int num_threads = min(hipBlockDim_x, N - offset); // Active threads in block
    for (int i = 0; i < 5; ++i) {
      output_offset[num_threads * i + hipThreadIdx_x] =
          output_shared[num_threads * i + hipThreadIdx_x];
    }
  }
}

} // namespace

template <>
bool RMACRegionsOp<HIPContext>::RunOnDevice() {
  const auto& X = Input(0); // Input tensor
  auto* output = Output(0); // RoIs

  if (X.size() == 0) {
    return true;
  }

  int batch_size = X.dim32(0);
  int H = X.dim32(2);
  int W = X.dim32(3);

  // Compute number of regions
  int min_step = 1;
  int max_step = 6;
  num_rois_.Resize(3); // num_rois, Wd, Hd
  hipLaunchKernelGGL((NumRMACRegionsKernel), dim3(1), dim3(CAFFE_HIP_NUM_THREADS), 0, context_.hip_stream(), 
      static_cast<const int>(W),
      static_cast<const int>(H),
      static_cast<const int>(min_step),
      static_cast<const int>(max_step),
      overlap_,
      scales_,
      num_rois_.mutable_data<int>());

  // Bit awkward, but the size of the output tensor depends on the output of
  // NumRMACRegionsKernel (number of RoIs), so need to copy that to CPU
  // to Resize() output appropriately.
  int num_rois = 0;
  context_.CopyBytes<HIPContext, CPUContext>(
      sizeof(int), num_rois_.data<int>(), &num_rois);
  int N = batch_size * num_rois;
  output->Resize(N, 5); // [batch_id x1 y1 x2 y2]

  // Compute region coordinates
  hipLaunchKernelGGL((RMACRegionsKernel), dim3(CAFFE_GET_BLOCKS(N)), dim3(CAFFE_HIP_NUM_THREADS), 0, context_.hip_stream(), 
      static_cast<const int>(W), static_cast<const int>(H), static_cast<const int>(N), num_rois_.data<int>(), output->mutable_data<float>());

  return true;
}

REGISTER_HIP_OPERATOR(RMACRegions, RMACRegionsOp<HIPContext>);

} // namespace caffe2
