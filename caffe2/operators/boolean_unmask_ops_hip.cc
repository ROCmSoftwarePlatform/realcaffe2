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

#include "caffe2/core/context_hip.h"
#include "caffe2/operators/boolean_unmask_ops.h"
#include "hip/hip_runtime.h"

namespace caffe2 {

namespace {

__global__ void ComputeIndicesKernel(
    const int numMasks,
    const int maskSize,
    int* indices,
    bool* const masks[]) {
  HIP_1D_KERNEL_LOOP(i, maskSize) {
    for (int j = 0; j < numMasks; ++j) {
      if (masks[j][i]) {
        indices[i] = j;
        return;
      }
    }
    HIP_KERNEL_ASSERT(false);
  }
}

__global__ void FillValuesKernel(
    const int numMasks,
    const int maskSize,
    const size_t itemSize,
    const int* indices,
    char* const values[],
    int valueSizes[],
    char* dest) {
  HIP_1D_KERNEL_LOOP(j, numMasks) {
    int k = 0;
    for (int i = 0; i < maskSize; ++i) {
      if (indices[i] == j) {
        for (int h = 0; h < itemSize; ++h) {
          dest[i * itemSize + h] = values[j][k * itemSize + h];
        }
        ++k;
      }
    }
    HIP_KERNEL_ASSERT(valueSizes[j] == k);
  }
}
 
} // namespace

template <>
class BooleanUnmaskOp<HIPContext> final : public Operator<HIPContext> {
 public:
  BooleanUnmaskOp(const OperatorDef& def, Workspace* ws)
      : Operator<HIPContext>(def, ws) {}

  bool RunOnDevice() override {
    int maskSize = Input(0).size();
    int numMasks = InputSize() / 2;
    const auto& meta = Input(1).meta();

    auto* out = Output(0);
    out->Resize(maskSize);
    auto* dest = (char*)out->raw_mutable_data(meta);

    hostMasks_.Resize(numMasks);
    auto* hostMasksData = hostMasks_.mutable_data<bool*>();
    hostValues_.Resize(numMasks);
    auto* hostValuesData = hostValues_.mutable_data<char*>();
    hostValueSizes_.Resize(numMasks);
    auto* hostValueSizesData = hostValueSizes_.mutable_data<int>();
    for (int i = 0; i < numMasks; ++i) {
      auto& mask = Input(i * 2);
      CAFFE_ENFORCE_EQ(mask.ndim(), 1);
      CAFFE_ENFORCE_EQ(mask.size(), maskSize);
      hostMasksData[i] = const_cast<bool*>(mask.data<bool>());

      const auto& value = Input(i * 2 + 1);
      CAFFE_ENFORCE_EQ(value.ndim(), 1);
      hostValuesData[i] = (char*)value.raw_data();
      hostValueSizesData[i] = value.size();
    }
    masks_.CopyFrom(hostMasks_, &context_);
    values_.CopyFrom(hostValues_, &context_);
    valueSizes_.CopyFrom(hostValueSizes_, &context_);

    indices_.Resize(maskSize);
    auto* indicesData = indices_.mutable_data<int>();

    hipLaunchKernelGGL((ComputeIndicesKernel), dim3(min(maskSize, CAFFE_MAXIMUM_NUM_BLOCKS)), dim3(CAFFE_HIP_NUM_THREADS), 0, context_.hip_stream(), 
        static_cast<const int>(numMasks), static_cast<const int>(maskSize), indicesData, masks_.data<bool*>());

    auto* valueSizesData = valueSizes_.mutable_data<int>();
    hipLaunchKernelGGL((FillValuesKernel), dim3(min(numMasks, CAFFE_MAXIMUM_NUM_BLOCKS)), dim3(CAFFE_HIP_NUM_THREADS), 0, context_.hip_stream(), 
        static_cast<const int>(numMasks),
        static_cast<const int>(maskSize),
        meta.itemsize(),
        indicesData,
        values_.data<char*>(),
        valueSizesData,
        dest);

    return true;
  }

 private:
  Tensor<HIPContext> indices_;
  Tensor<HIPContext> masks_;
  Tensor<HIPContext> values_;
  Tensor<HIPContext> valueSizes_;

  Tensor<CPUContext> hostMasks_;
  Tensor<CPUContext> hostValues_;
  Tensor<CPUContext> hostValueSizes_;
};

REGISTER_HIP_OPERATOR(BooleanUnmask, BooleanUnmaskOp<HIPContext>);

} // caffe2
