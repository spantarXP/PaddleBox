/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/framework/device_worker.h"

DECLARE_bool(lineid_have_extend_info);
DECLARE_bool(dump_filed_same_as_aibox);
namespace paddle {
namespace framework {

class LoDTensor;
class Scope;

void DeviceWorker::SetRootScope(Scope* root_scope) { root_scope_ = root_scope; }

void DeviceWorker::SetDataFeed(DataFeed* data_feed) {
  device_reader_ = data_feed;
}

template <typename T>
void PrintLodTensorType(Tensor* tensor, int64_t start, int64_t end,
                        std::ostringstream* os) {
  auto count = tensor->numel();
  if (start < 0 || end > count) {
    VLOG(3) << "access violation";
    (*os) << "access violation";
    return;
  }
  for (int64_t i = start; i < end; i++) {
    (*os) << ":" << tensor->data<T>()[i];
  }
}

void PrintLodTensorIntType(Tensor* tensor, int64_t start, int64_t end,
                           std::ostringstream* os) {
  auto count = tensor->numel();
  if (start < 0 || end > count) {
    VLOG(3) << "access violation";
    (*os) << "access violation";
    return;
  }
  for (int64_t i = start; i < end; i++) {
    (*os) << ":" << static_cast<uint64_t>(tensor->data<int64_t>()[i]);
  }
}
void PrintLodTensor(Tensor* tensor, int64_t start, int64_t end,
                    std::ostringstream* os) {
  if (tensor->type() == proto::VarType::FP32) {
    PrintLodTensorType<float>(tensor, start, end, os);
  } else if (tensor->type() == proto::VarType::INT64) {
    PrintLodTensorIntType(tensor, start, end, os);
  } else if (tensor->type() == proto::VarType::FP64) {
    PrintLodTensorType<double>(tensor, start, end, os);
  } else if (tensor->type() == proto::VarType::INT32) {
    PrintLodTensorType<int>(tensor, start, end, os);
  } else if (tensor->type() == proto::VarType::INT16) {
    PrintLodTensorType<int16_t>(tensor, start, end, os);
  } else {
    (*os) << "unsupported type";
  }
}

std::pair<int64_t, int64_t> GetTensorBound(LoDTensor* tensor, int index) {
  auto& dims = tensor->dims();
  if (tensor->lod().size() != 0) {
    auto& lod = tensor->lod()[0];
    return {lod[index] * dims[1], lod[index + 1] * dims[1]};
  } else {
    return {index * dims[1], (index + 1) * dims[1]};
  }
}

bool CheckValidOutput(LoDTensor* tensor, size_t batch_size) {
  auto& dims = tensor->dims();
  if (dims.size() != 2) return false;
  if (tensor->lod().size() != 0) {
    auto& lod = tensor->lod()[0];
    if (lod.size() != batch_size + 1) {
      return false;
    }
  } else {
    if (dims[0] != static_cast<int>(batch_size)) {
      return false;
    }
  }
  return true;
}

void DeviceWorker::DumpParam(const Scope& scope, const int batch_id) {
  std::ostringstream os;
  for (auto& param : *dump_param_) {
    os.str("");
    Variable* var = scope.FindVar(param);
    if (var == nullptr) {
      continue;
    }
    LoDTensor* tensor = var->GetMutable<LoDTensor>();
    framework::LoDTensor cpu_tensor;
    if (platform::is_gpu_place(tensor->place())) {
      TensorCopySync(*tensor, platform::CPUPlace(), &cpu_tensor);
      tensor = &cpu_tensor;
    }
    int64_t len = tensor->numel();
    os << "(" << batch_id << "," << param << ")";
    PrintLodTensor(tensor, 0, len, &os);
    writer_ << os.str();
  }
}

void DeviceWorker::InitRandomDumpConfig(const TrainerDesc& desc) {
  bool enable_random_dump = desc.enable_random_dump();
  if (!enable_random_dump) {
    dump_mode_ = 0;
  } else {
    if (desc.random_with_lineid()) {
      dump_mode_ = 1;
    } else {
      dump_mode_ = 2;
    }
  }
  dump_interval_ = desc.dump_interval();
}

void DeviceWorker::DumpField(const Scope& scope, int dump_mode,
                             int dump_interval) {  // dump_mode: 0: no random,
                                                   // 1: random with insid hash,
                                                   // 2: random with random
                                                   // number
  size_t batch_size = device_reader_->GetCurBatchSize();
  std::vector<std::ostringstream> ars(batch_size);
  std::vector<bool> hit(batch_size, false);

  std::default_random_engine engine(0);
  std::uniform_int_distribution<size_t> dist(0U, INT_MAX);
  for (size_t i = 0; i < batch_size; i++) {
    size_t r = 0;
    const std::string& lineid = device_reader_->GetLineId(i);
    if (dump_mode == 1) {
      r = XXH64(lineid.data(), lineid.length(), 0);
    } else if (dump_mode == 2) {
      r = dist(engine);
    }
    if (r % dump_interval != 0) {
      continue;
    }
    hit[i] = true;
    if (FLAGS_lineid_have_extend_info) {
      size_t pos = lineid.find(" ");
      if (pos != std::string::npos) {
        ars[i] << lineid.substr(0, pos);
      } else {
        ars[i] << lineid;
      }
    } else {
      ars[i] << lineid;
    }
  }
  for (auto& field : *dump_fields_) {
    Variable* var = scope.FindVar(field);
    if (var == nullptr) {
      VLOG(0) << "Note: field[" << field
              << "] cannot be find in scope, so it was skipped.";
      continue;
    }
    LoDTensor* tensor = var->GetMutable<LoDTensor>();
    if (!tensor->IsInitialized()) {
      VLOG(0) << "Note: field[" << field
              << "] is not initialized, so it was skipped.";
      continue;
    }
    framework::LoDTensor cpu_tensor;
    if (platform::is_gpu_place(tensor->place())) {
      TensorCopySync(*tensor, platform::CPUPlace(), &cpu_tensor);
      cpu_tensor.set_lod(tensor->lod());
      tensor = &cpu_tensor;
    }
    if (!CheckValidOutput(tensor, batch_size)) {
      VLOG(0) << "Note: field[" << field << "] cannot pass check, so it was "
                                            "skipped. Maybe the dimension is "
                                            "wrong ";
      continue;
    }
    for (size_t i = 0; i < batch_size; ++i) {
      if (!hit[i]) {
        continue;
      }
      auto bound = GetTensorBound(tensor, i);
      if (FLAGS_dump_filed_same_as_aibox) {
        size_t pos = field.find(".");
        std::string new_field = field;
        if (pos != std::string::npos) {
          new_field = field.substr(0, pos);
        }
        ars[i] << "\t" << new_field;
      } else {
        ars[i] << "\t" << field << ":"
               << std::to_string(bound.second - bound.first);
      }
      PrintLodTensor(tensor, bound.first, bound.second, &ars[i]);
    }
  }
  // #pragma omp parallel for
  for (size_t i = 0; i < ars.size(); i++) {
    if (ars[i].tellp() <= 0) {
      continue;
    }

    if (FLAGS_lineid_have_extend_info) {
      const std::string& lineid = device_reader_->GetLineId(i);
      size_t pos = lineid.find(" ");
      if (pos != std::string::npos) {
        ars[i] << "\t" << lineid.substr(pos + 1);
      }
    }
    writer_ << ars[i].str();
  }
}

}  // namespace framework
}  // namespace paddle
