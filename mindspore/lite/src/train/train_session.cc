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

#include "src/train/train_session.h"
#include <algorithm>
#include <utility>
#include <vector>
#include "include/errorcode.h"
#include "include/train_model.h"
#include "src/common/utils.h"
#include "src/tensor.h"
#include "src/train/loss_kernel.h"
#include "src/train/train_populate_parameter.h"
#include "src/runtime/runtime_api.h"
#include "src/executor.h"
#include "src/kernel_registry.h"
#include "src/runtime/kernel/arm/fp32_grad/convolution.h"

namespace mindspore {
namespace lite {

static size_t TSFindTensor(const std::vector<lite::Tensor *> &where, const lite::Tensor *searchParameter) {
  for (size_t i = 0; i < where.size(); i++) {
    if (where[i] == searchParameter) {
      return i;
    }
  }
  return where.size();
}

TrainSession::TrainSession() { kernel::PopulateTrainParameters(); }

std::vector<CreatorOp> TrainSession::ReplaceOps() {
  const std::vector<CreatorOp> replace = {
    {{mindspore::kernel::KERNEL_ARCH::kCPU, kNumberTypeFloat32, mindspore::schema::PrimitiveType_Conv2D},
     mindspore::kernel::CpuConvTrainFp32KernelCreator},
    {{mindspore::kernel::KERNEL_ARCH::kCPU, kNumberTypeFloat32, mindspore::schema::PrimitiveType_DepthwiseConv2D},
     mindspore::kernel::CpuConvTrainFp32KernelCreator}};
  mindspore::lite::KernelRegistry *reg = mindspore::lite::KernelRegistry::GetInstance();
  std::vector<CreatorOp> results;
  for (auto v : replace) {
    const CreatorOp cl = make_tuple(std::get<0>(v), reg->GetCreator(std::get<0>(v)));
    results.push_back(cl);
    reg->RegKernel(std::get<0>(v), std::get<1>(v));
  }
  return results;
}

void TrainSession::RestoreOps(const std::vector<CreatorOp> &restore) {
  mindspore::lite::KernelRegistry *reg = mindspore::lite::KernelRegistry::GetInstance();
  for (auto v : restore) {
    reg->RegKernel(std::get<0>(v), std::get<1>(v));
  }
}

void TrainSession::AllocWorkSpace() {
  size_t workspace_size = 0;
  for (auto k : kernels_) {
    if (workspace_size < k->GetWorkspaceSize()) {
      workspace_size = k->GetWorkspaceSize();
    }
  }
  mindspore::kernel::LiteKernel::AllocWorkspace(workspace_size);
}

int TrainSession::CompileGraph(lite::Model *model) { return lite::RET_ERROR; }

int TrainSession::CompileTrainGraph(mindspore::lite::TrainModel *model) {
  model_ = model;

  auto restore = ReplaceOps();
  auto ret = lite::LiteSession::CompileGraph(model);
  orig_output_map_ = output_node_map_;
  orig_output_tensor_map_ = output_tensor_map_;
  for (auto inTensor : inputs_) inTensor->MutableData();
  RestoreOps(restore);
  AllocWorkSpace();
  return ret;
}

TrainSession::~TrainSession() {
  mindspore::kernel::LiteKernel::FreeWorkspace();
  delete model_;
}

void *TrainSession::ExportToBuf(char *buf, size_t *len) const { return model_->ExportBuf(buf, len); }

int TrainSession::RunGraph(const session::KernelCallBack &before, const session::KernelCallBack &after) {
  this->outputs_.clear();
  for (auto ms_tensors : output_node_map_)
    for (auto ms_tensor : ms_tensors.second) this->outputs_.push_back((static_cast<lite::Tensor *>(ms_tensor)));
  if (train_mode_) return lite::LiteSession::RunGraph(before, after);

  // object is expected to run only inference part of graph
  // prepare a list of kernels till the loss function -- temporary solution
  std::vector<kernel::LiteKernel *> inference_kernels;
  for (auto kernel : this->kernels_) {
    if (IsLossKernel(kernel)) break;
    inference_kernels.push_back(kernel);
  }

  if (this->context_ == nullptr) {
    MS_LOG(ERROR) << "context is null";
    return lite::RET_NULL_PTR;
  }
  lite::Executor executor;
  if (before == nullptr && after == nullptr) {
    return executor.Run(this->inputs_, this->outputs_, inference_kernels, this->context_->allocator.get());
  } else {
    return executor.Run(this->inputs_, this->outputs_, inference_kernels, this->context_->allocator.get(), before,
                        after);
  }
}

void TrainSession::Train() {
  for (auto *kernel : kernels_) {
    MS_ASSERT(nullptr != kernel);
    kernel->train();
  }
  output_node_map_.clear();
  output_tensor_map_.clear();
  train_mode_ = true;
  for (auto kernel : this->kernels_) {
    if (IsLossKernel(kernel)) {
      auto *ms_tensor = kernel->out_tensors().at(0);
      if (ms_tensor != nullptr) {
        ms_tensor->MutableData();
        output_node_map_[kernel->name()].emplace_back(ms_tensor);
        auto index = TSFindTensor(tensors_, ms_tensor);
        if (index != tensors_.size()) {
          output_tensor_map_.insert(std::make_pair(std::to_string(index), ms_tensor));
        }
      }
    }
  }
}

void TrainSession::Eval() {
  for (auto *kernel : this->kernels_) {
    MS_ASSERT(nullptr != kernel);
    kernel->eval();
  }
  output_node_map_ = orig_output_map_;
  output_tensor_map_ = orig_output_tensor_map_;

  train_mode_ = false;
  for (auto kernel : this->kernels_) {
    if (IsLossKernel(kernel)) {
      for (auto in_kernel : kernel->in_kernels()) {
        if (output_node_map_.find(in_kernel->name()) == output_node_map_.end()) {
          auto *ms_tensor = in_kernel->out_tensors().at(0);
          if (ms_tensor != nullptr) {
            output_node_map_[in_kernel->name()].emplace_back(ms_tensor);
            auto index = TSFindTensor(tensors_, ms_tensor);
            if (index != tensors_.size()) {
              output_tensor_map_.insert(std::make_pair(std::to_string(index), ms_tensor));
            }
          }
        }
      }
    }
  }
}

bool TrainSession::IsLossKernel(kernel::LiteKernel *kernel) {
  return (kernel->Type() == schema::PrimitiveType_SoftmaxCrossEntropy);
}

}  // namespace lite

session::TrainSession *session::TrainSession::CreateSession(lite::Context *context) {
  auto session = new lite::TrainSession();
  auto ret = session->Init(context);
  if (ret != mindspore::lite::RET_OK) {
    MS_LOG(ERROR) << "init sesssion failed";
    delete session;
    return nullptr;
  }
  return session;
}

}  // namespace mindspore
