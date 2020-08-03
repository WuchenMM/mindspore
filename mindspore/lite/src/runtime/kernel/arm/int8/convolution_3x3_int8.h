/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_INT8_CONVOLUTION_3X3_INT8_H_
#define MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_INT8_CONVOLUTION_3X3_INT8_H_

#include <vector>
#include "src/lite_kernel.h"

#include "src/runtime/kernel/arm/opclib/winograd_transform.h"
#include "src/runtime/kernel/arm/base/convolution_base.h"

namespace mindspore::kernel {
class Convolution3x3Int8CPUKernel : public ConvolutionBaseCPUKernel {
 public:
  Convolution3x3Int8CPUKernel(OpParameter *parameter, const std::vector<lite::tensor::Tensor *> &inputs,
                              const std::vector<lite::tensor::Tensor *> &outputs, const Context *ctx)
      : ConvolutionBaseCPUKernel(parameter, inputs, outputs, ctx) {}
  ~Convolution3x3Int8CPUKernel() override;

  int Init() override;
  int ReSize() override;
  int Run() override;
  int RunImpl(int task_id);
  int InitWeightBias();
  int InitTmpBuffer();
  void ConfigInputOutput();

 private:
  int16_t *transformed_filter_addr_;
  int16_t *input_data_;
  int16_t *tile_buffer_;
  int16_t *block_unit_buffer_;
  int32_t *tmp_dst_buffer_;
  int8_t *tmp_out_;
};
void ProcessFilterUint8(int8_t *origin_weight, int16_t *dst_weight, ConvParameter *conv_param);
}  // namespace mindspore::kernel

#endif  // MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_INT8_CONVOLUTION_3X3_INT8_H_
