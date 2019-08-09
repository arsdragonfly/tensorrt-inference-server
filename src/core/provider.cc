// Copyright (c) 2018-2019, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/core/provider.h"

#include <deque>
#include <numeric>
#include "src/core/backend.h"
#include "src/core/constants.h"
#include "src/core/logging.h"
#include "src/core/model_config.h"
#include "src/core/model_config_utils.h"

#ifdef TRTIS_ENABLE_GPU
#include <cuda_runtime_api.h>
#endif  // TRTIS_ENABLE_GPU

namespace nvidia { namespace inferenceserver {

//
// SystemMemoryReference
//
SystemMemoryReference::SystemMemoryReference() : SystemMemory() {}

const char*
SystemMemoryReference::BufferAt(
    size_t idx, size_t* byte_size, TRTSERVER_Memory_Type* memory_type) const
{
  if (idx >= buffer_.size()) {
    *byte_size = 0;
    *memory_type = TRTSERVER_MEMORY_CPU;
    return nullptr;
  }
  *memory_type = std::get<2>(buffer_[idx]);
  *byte_size = std::get<1>(buffer_[idx]);
  return std::get<0>(buffer_[idx]);
}

size_t
SystemMemoryReference::AddBuffer(
    const char* buffer, size_t byte_size, TRTSERVER_Memory_Type memory_type)
{
  total_byte_size_ += byte_size;
  buffer_.emplace_back(std::make_tuple(buffer, byte_size, memory_type));
  return buffer_.size() - 1;
}

AllocatedSystemMemory::AllocatedSystemMemory(
    size_t byte_size, TRTSERVER_Memory_Type memory_type)
    : SystemMemory(), memory_type_(memory_type)
{
  total_byte_size_ = byte_size;
  if (memory_type_ == TRTSERVER_MEMORY_CPU) {
    buffer_ = new char[byte_size];
  } else {
#ifdef TRTIS_ENABLE_GPU
    cudaError_t err = cudaMalloc((void**)&buffer_, byte_size);
    if (err != cudaSuccess) {
      LOG_ERROR << "failed to allocate GPU memory with byte size" << byte_size
                << ": " << std::string(cudaGetErrorString(err));
      buffer_ = nullptr;
    }
#else
    buffer_ = nullptr;
#endif  // TRTIS_ENABLE_GPU
  }
}

AllocatedSystemMemory::~AllocatedSystemMemory()
{
  if (buffer_ != nullptr) {
    if (memory_type_ == TRTSERVER_MEMORY_CPU) {
      delete buffer_;
    } else {
#ifdef TRTIS_ENABLE_GPU
      cudaError_t err = cudaFree(buffer_);
      if (err != cudaSuccess) {
        LOG_ERROR << "failed to free GPU memory at address " << buffer_ << ": "
                  << std::string(cudaGetErrorString(err));
      }
#endif  // TRTIS_ENABLE_GPU
    }
    buffer_ = nullptr;
  }
}

const char*
AllocatedSystemMemory::BufferAt(
    size_t idx, size_t* byte_size, TRTSERVER_Memory_Type* memory_type) const
{
  if (idx != 0) {
    *byte_size = 0;
    *memory_type = TRTSERVER_MEMORY_CPU;
    return nullptr;
  }
  *byte_size = total_byte_size_;
  *memory_type = memory_type_;
  return buffer_;
}

char*
AllocatedSystemMemory::MutableBuffer(TRTSERVER_Memory_Type* memory_type)
{
  *memory_type = memory_type_;
  return buffer_;
}

//
// InferRequestProvider
//
Status
InferRequestProvider::Create(
    const std::string& model_name, const int64_t model_version,
    const InferRequestHeader& request_header,
    const std::unordered_map<std::string, std::shared_ptr<SystemMemory>>&
        input_buffer,
    std::shared_ptr<InferRequestProvider>* provider)
{
  provider->reset(new InferRequestProvider(model_name, model_version));

  (*provider)->request_header_ = request_header;

  for (const auto& io : request_header.input()) {
    auto it = input_buffer.find(io.name());
    if (it == input_buffer.end()) {
      return Status(
          RequestStatusCode::INVALID_ARG,
          "input '" + io.name() + "' is specified in request header but" +
              " not found in memory block mapping for model '" +
              (*provider)->model_name_ + "'");
    }
    if (io.batch_byte_size() != it->second->TotalByteSize()) {
      return Status(
          RequestStatusCode::INVALID_ARG,
          "unexpected size " + std::to_string(it->second->TotalByteSize()) +
              " for input '" + io.name() + "', expecting " +
              std::to_string(io.batch_byte_size()) + " for model '" +
              (*provider)->model_name_ + "'");
    }
    (*provider)->input_buffer_[io.name()] = std::make_pair(it->second, 0);
  }

  // for (const auto& io : request_header.output()) {
  //   auto it = output_shm_buffer.find(io.name());
  //   if (it == output_shm_buffer.end()) {
  //     return Status(
  //         RequestStatusCode::INVALID_ARG,
  //         "output '" + io.name() + "' is specified in request header but" +
  //             " not found in memory block mapping for model '" +
  //             (*provider)->model_name_ + "'");
  //   }
  //   (*provider)->output_shm_buffer_[io.name()] = std::make_pair(it->second,
  //   0);
  // }

  return Status::Success;
}

const std::shared_ptr<InferRequestProvider::InputOverrideMap>&
InferRequestProvider::GetInputOverride() const
{
  return overrides_;
}

Status
InferRequestProvider::SetInputOverride(
    const std::shared_ptr<InputOverrideMap>& override)
{
  overrides_ = override;
  return Status::Success;
}

bool
InferRequestProvider::GetInputOverrideContent(
    const std::string& name, const void** content, size_t* content_byte_size)
{
  if (overrides_ != nullptr) {
    const auto& pr = overrides_->find(name);
    if (pr != overrides_->end()) {
      if ((*content_byte_size == 0) ||
          (overrides_consumed_.find(name) != overrides_consumed_.end())) {
        *content = nullptr;
        *content_byte_size = 0;
      } else {
        std::shared_ptr<InputOverride>& override = pr->second;
        *content = reinterpret_cast<void*>(&(override->content_[0]));
        *content_byte_size = override->content_.size();
        overrides_consumed_.insert(name);
      }

      return true;
    }
  }

  return false;
}

Status
InferRequestProvider::GetNextInputContent(
    const std::string& name, const void** content, size_t* content_byte_size,
    TRTSERVER_Memory_Type* memory_type, bool force_contiguous)
{
  if (*content_byte_size == 0) {
    *content = nullptr;
    return Status::Success;
  }

  if (!GetInputOverrideContent(name, content, content_byte_size)) {
    const auto& pr = input_buffer_.find(name);
    if (pr == input_buffer_.end()) {
      return Status(
          RequestStatusCode::INTERNAL, "unexpected input '" + name + "'");
    }

    auto& input_content = pr->second;

    bool isLastChunk =
        (input_content.first->BufferAt(
             input_content.second + 1, content_byte_size, memory_type) ==
         nullptr);
    if (!force_contiguous || isLastChunk) {
      *content = input_content.first->BufferAt(
          input_content.second++, content_byte_size, memory_type);
    } else {
      size_t total_size = 0;
      size_t start_idx = input_content.second;
      do {
        *content = input_content.first->BufferAt(
            input_content.second++, content_byte_size, memory_type);
        total_size += *content_byte_size;
      } while (*content != nullptr);

      contiguous_buffers_.emplace_back();
      std::vector<char>& buf = contiguous_buffers_.back();
      buf.reserve(total_size);

      // [TODO] on 'force_contiguous', need to be careful as the data block
      // may be on GPU
      for (size_t i = start_idx; i < input_content.second; i++) {
        const auto& block =
            input_content.first->BufferAt(i, content_byte_size, memory_type);
        buf.insert(buf.end(), block, block + *content_byte_size);
      }

      if (buf.size() != total_size) {
        return Status(RequestStatusCode::INTERNAL, "contiguous input failed");
      }

      *content = &(buf[0]);
      *content_byte_size = total_size;
    }
  }

  return Status::Success;
}

Status
InferRequestProvider::GetSystemMemory(
    const std::string& name, std::shared_ptr<SystemMemory>* input_buffer)
{
  auto it = input_buffer_.find(name);
  if (it == input_buffer_.end()) {
    return Status(
        RequestStatusCode::INVALID_ARG,
        "input '" + name + "' is not found in the provider");
  }
  *input_buffer = it->second.first;
  return Status::Success;
}

//
// NULLInferRequestProvider
//
std::vector<uint8_t> NULLInferRequestProvider::buf_;
std::mutex NULLInferRequestProvider::mu_;

Status
NULLInferRequestProvider::GetNextInputContent(
    const std::string& name, const void** content, size_t* content_byte_size,
    TRTSERVER_Memory_Type* memory_type, bool force_contiguous)
{
  *memory_type = TRTSERVER_MEMORY_CPU;
  if (*content_byte_size == 0) {
    *content = nullptr;
    return Status::Success;
  }

  if (!GetInputOverrideContent(name, content, content_byte_size)) {
    std::lock_guard<std::mutex> lock(mu_);

    // Must return content with all zero data. This is required by
    // string-datatype tensors where it is interpreted as all empty
    // strings. Clamp the maximum size that we allow the buffer to
    // grow to avoid massive allocation.
    if (buf_.size() < *content_byte_size) {
      constexpr size_t max_size = 16 * 1024 * 1024;
      buf_.resize(std::min(max_size, *content_byte_size), 0);
    }

    *content = &(buf_[0]);
  }

  return Status::Success;
}

namespace {

template <typename T>
void
AddClassResults(
    InferResponseHeader::Output* poutput, char* poutput_buffer,
    const size_t batch1_element_count, const size_t batch_size,
    const size_t cls_count,
    const std::shared_ptr<LabelProvider>& label_provider,
    const InferResponseProvider::SecondaryLabelProviderMap& lookup_map)
{
  T* probs = reinterpret_cast<T*>(poutput_buffer);
  const size_t entry_cnt = batch1_element_count;
  const size_t class_cnt = std::min(cls_count, entry_cnt);
  std::vector<size_t> idx(entry_cnt);

  for (size_t i = 0; i < batch_size; ++i) {
    iota(idx.begin(), idx.end(), 0);
    sort(idx.begin(), idx.end(), [&probs](size_t i1, size_t i2) {
      return probs[i1] > probs[i2];
    });

    auto bcls = poutput->add_batch_classes();
    for (size_t k = 0; k < class_cnt; ++k) {
      auto cls = bcls->add_cls();
      cls->set_idx(idx[k]);
      const auto& label = label_provider->GetLabel(poutput->name(), idx[k]);
      cls->set_label(label);

      if (label == "" && !lookup_map.empty()) {
        auto it = lookup_map.find(poutput->name());
        if (it != lookup_map.end()) {
          cls->set_label(it->second.second->GetLabel(it->second.first, idx[k]));
        }
      }

      cls->set_value(static_cast<float>(probs[idx[k]]));
    }

    probs += entry_cnt;
  }
}

}  // namespace

//
// InferResponseProvider
//
InferResponseProvider::InferResponseProvider(
    const InferRequestHeader& request_header,
    const std::shared_ptr<LabelProvider>& label_provider,
    const std::unordered_map<std::string, std::shared_ptr<SystemMemory>>&
        output_shm_buffer)
    : request_header_(request_header), label_provider_(label_provider)
{
  // Create a map from output name to the InferRequestHeader::Output
  // object for that output.
  for (const InferRequestHeader::Output& output : request_header.output()) {
    output_map_.emplace(std::make_pair(output.name(), output));
  }

  // Create a copy of the output_shm_buffer map for the response provider
  output_shm_buffer_.insert(output_shm_buffer.begin(), output_shm_buffer.end());
}

bool
InferResponseProvider::RequiresOutput(const std::string& name)
{
  return output_map_.find(name) != output_map_.end();
}

Status
InferResponseProvider::OutputBufferContents(
    const std::string& name, const void** content, size_t* content_byte_size,
    TRTSERVER_Memory_Type* memory_type) const
{
  for (const auto& output : outputs_) {
    if ((name == output.name_) && (output.cls_count_ == 0)) {
      *content = output.ptr_;
      *content_byte_size = output.byte_size_;
      *memory_type = output.memory_type_;
      return Status::Success;
    }
  }

  return Status(
      RequestStatusCode::UNAVAILABLE,
      "request for unallocated output '" + name + "'");
}

Status
InferResponseProvider::CheckAndSetIfBufferedOutput(
    const std::string& name, void** content, size_t content_byte_size,
    const std::vector<int64_t>& content_shape, Output** output)
{
  const auto& pr = output_map_.find(name);
  if (pr == output_map_.end()) {
    return Status(
        RequestStatusCode::INTERNAL, "unexpected output '" + name + "'");
  }

  outputs_.emplace_back();
  Output* loutput = &(outputs_.back());
  loutput->name_ = name;
  loutput->shape_ = content_shape;
  loutput->cls_count_ = 0;
  loutput->byte_size_ = content_byte_size;

  // if output uses shared memory initialize pointer with appropriate address
  auto shm_pr = output_shm_buffer_.find(name);
  size_t byte_size;
  if (shm_pr != output_shm_buffer_.end()) {
    *content = const_cast<void*>(
        reinterpret_cast<const void*>(shm_pr->second->BufferAt(0, &byte_size)));
    // TODO verify content_byte_size == byte_size
    loutput->ptr_ = content;
  } else {
    loutput->ptr_ = nullptr;
  }

  if (pr->second.has_cls()) {
    loutput->cls_count_ = pr->second.cls().count();
    char* buffer = new char[content_byte_size];
    *content = static_cast<void*>(buffer);
    loutput->ptr_ = static_cast<void*>(buffer);
    loutput->buffer_.reset(buffer);
  }

  *output = loutput;

  return Status::Success;
}

bool
InferResponseProvider::GetSecondaryLabelProvider(
    const std::string& name, SecondaryLabelProvider* provider)
{
  auto it = secondary_label_provider_map_.find(name);
  if (it != secondary_label_provider_map_.end()) {
    *provider = it->second;
    return true;
  }
  return false;
}

void
InferResponseProvider::SetSecondaryLabelProvider(
    const std::string& name, const SecondaryLabelProvider& provider)
{
  secondary_label_provider_map_[name] = provider;
}

Status
InferResponseProvider::FinalizeResponse(const InferenceBackend& is)
{
  InferResponseHeader* response_header = MutableResponseHeader();
  response_header->Clear();

  response_header->set_model_name(is.Name());
  response_header->set_model_version(is.Version());

  const size_t batch_size = request_header_.batch_size();
  response_header->set_batch_size(batch_size);

  int output_idx = 0;
  for (const auto& output : outputs_) {
    const ModelOutput* output_config;
    RETURN_IF_ERROR(is.GetOutput(output.name_, &output_config));

    // Verify that the actual output shape matches what is expected by
    // the model configuration. If there is an output reshape, we've
    // already verified that reshape and dims have same element count
    // so don't need to do that here.
    bool skip_batch = (is.Config().max_batch_size() != 0);
    DimsList batch1_backend_shape;
    size_t batch1_element_count = 1;
    for (auto d : output.shape_) {
      if (!skip_batch) {
        batch1_backend_shape.Add(d);
        batch1_element_count *= (size_t)d;
      }
      skip_batch = false;
    }

    const DimsList& expected_shape = (output_config->has_reshape())
                                         ? output_config->reshape().shape()
                                         : output_config->dims();
    if (!CompareDimsWithWildcard(expected_shape, batch1_backend_shape)) {
      return Status(
          RequestStatusCode::INVALID_ARG,
          "output '" + output.name_ + "' for model '" + is.Name() +
              "' has shape " + DimsListToString(batch1_backend_shape) +
              " but model configuration specifies shape " +
              DimsListToString(expected_shape));
    }

    auto poutput = response_header->add_output();
    poutput->set_name(output.name_);

    if (output.cls_count_ == 0) {
      // Raw result...
      poutput->mutable_raw()->Clear();
      poutput->mutable_raw()->set_batch_byte_size(output.byte_size_);

      // If there is a reshape them we need to record corresponding value for
      // variable-size dimensions so that we can set the output shape correctly.
      // If there is not a reshape then use output shape as that will have
      // actual sized in place of any wildcard dimensions.
      if (output_config->has_reshape()) {
        std::deque<int64_t> variable_size_values;
        for (int64_t idx = 0; idx < output_config->reshape().shape_size();
             idx++) {
          if (output_config->reshape().shape(idx) == -1) {
            variable_size_values.push_back(batch1_backend_shape[idx]);
          }
        }

        for (const auto& dim : output_config->dims()) {
          if (dim == -1) {
            poutput->mutable_raw()->add_dims(variable_size_values.front());
            variable_size_values.pop_front();
          } else {
            poutput->mutable_raw()->add_dims(dim);
          }
        }
      } else {
        poutput->mutable_raw()->mutable_dims()->CopyFrom(batch1_backend_shape);
      }
    } else {
      // Class result...
      switch (output_config->data_type()) {
        case DataType::TYPE_UINT8:
          AddClassResults<uint8_t>(
              poutput, output.buffer_.get(), batch1_element_count, batch_size,
              output.cls_count_, label_provider_,
              secondary_label_provider_map_);
          break;
        case DataType::TYPE_UINT16:
          AddClassResults<uint16_t>(
              poutput, output.buffer_.get(), batch1_element_count, batch_size,
              output.cls_count_, label_provider_,
              secondary_label_provider_map_);
          break;
        case DataType::TYPE_UINT32:
          AddClassResults<uint32_t>(
              poutput, output.buffer_.get(), batch1_element_count, batch_size,
              output.cls_count_, label_provider_,
              secondary_label_provider_map_);
          break;
        case DataType::TYPE_UINT64:
          AddClassResults<uint64_t>(
              poutput, output.buffer_.get(), batch1_element_count, batch_size,
              output.cls_count_, label_provider_,
              secondary_label_provider_map_);
          break;

        case DataType::TYPE_INT8:
          AddClassResults<int8_t>(
              poutput, output.buffer_.get(), batch1_element_count, batch_size,
              output.cls_count_, label_provider_,
              secondary_label_provider_map_);
          break;
        case DataType::TYPE_INT16:
          AddClassResults<int16_t>(
              poutput, output.buffer_.get(), batch1_element_count, batch_size,
              output.cls_count_, label_provider_,
              secondary_label_provider_map_);
          break;
        case DataType::TYPE_INT32:
          AddClassResults<int32_t>(
              poutput, output.buffer_.get(), batch1_element_count, batch_size,
              output.cls_count_, label_provider_,
              secondary_label_provider_map_);
          break;
        case DataType::TYPE_INT64:
          AddClassResults<int64_t>(
              poutput, output.buffer_.get(), batch1_element_count, batch_size,
              output.cls_count_, label_provider_,
              secondary_label_provider_map_);
          break;

        case DataType::TYPE_FP32:
          AddClassResults<float>(
              poutput, output.buffer_.get(), batch1_element_count, batch_size,
              output.cls_count_, label_provider_,
              secondary_label_provider_map_);
          break;
        case DataType::TYPE_FP64:
          AddClassResults<double>(
              poutput, output.buffer_.get(), batch1_element_count, batch_size,
              output.cls_count_, label_provider_,
              secondary_label_provider_map_);
          break;

        default:
          return Status(
              RequestStatusCode::INVALID_ARG,
              "class result not available for output '" + output.name_ +
                  "' due to unsupported type '" +
                  DataType_Name(output_config->data_type()) + "'");
      }
    }

    output_idx++;
  }

  return Status::Success;
}

//
// InternalInferResponseProvider
//
Status
InternalInferResponseProvider::Create(
    const InferenceBackend& is, const InferRequestHeader& request_header,
    const std::shared_ptr<LabelProvider>& label_provider,
    const std::unordered_map<std::string, std::shared_ptr<SystemMemory>>&
        output_shm_buffer,
    std::shared_ptr<InternalInferResponseProvider>* infer_provider)
{
  auto provider = new InternalInferResponseProvider(
      request_header, label_provider, output_shm_buffer);
  infer_provider->reset(provider);
  return Status::Success;
}

const InferResponseHeader&
InternalInferResponseProvider::ResponseHeader() const
{
  return response_header_;
}

InferResponseHeader*
InternalInferResponseProvider::MutableResponseHeader()
{
  return &response_header_;
}

Status
InternalInferResponseProvider::AllocateOutputBuffer(
    const std::string& name, void** content, size_t content_byte_size,
    const std::vector<int64_t>& content_shape)
{
  *content = nullptr;

  Output* output;
  RETURN_IF_ERROR(CheckAndSetIfBufferedOutput(
      name, content, content_byte_size, content_shape, &output));

  // Always write output tensor to an output buffer no matter
  // if output has cls field defined
  auto it = output_buffer_.find(name);
  if (it == output_buffer_.end()) {
    it = output_buffer_
             .emplace(std::make_pair(
                 name, std::make_shared<AllocatedSystemMemory>(
                           content_byte_size, TRTSERVER_MEMORY_CPU)))
             .first;
  }

  if (content_byte_size != it->second->TotalByteSize()) {
    return Status(
        RequestStatusCode::INVALID_ARG,
        "unexpected size " + std::to_string(it->second->TotalByteSize()) +
            " for output '" + name + "', expecting " +
            std::to_string(content_byte_size));
  }

  TRTSERVER_Memory_Type memory_type;
  *content = it->second->MutableBuffer(&memory_type);
  output->ptr_ = *content;

  return Status::Success;
}

Status
InternalInferResponseProvider::GetSystemMemory(
    const std::string& name, std::shared_ptr<SystemMemory>* output_buffer)
{
  auto it = output_buffer_.find(name);
  if (it == output_buffer_.end()) {
    return Status(
        RequestStatusCode::INVALID_ARG,
        "output '" + name + "' is not found in response provider");
  }
  *output_buffer = std::static_pointer_cast<SystemMemory>(it->second);
  return Status::Success;
}

InternalInferResponseProvider::InternalInferResponseProvider(
    const InferRequestHeader& request_header,
    const std::shared_ptr<LabelProvider>& label_provider,
    const std::unordered_map<std::string, std::shared_ptr<SystemMemory>>&
        output_shm_buffer)
    : InferResponseProvider(request_header, label_provider, output_shm_buffer)
{
}

//
// GRPCInferResponseProvider
//
Status
GRPCInferResponseProvider::Create(
    const InferRequestHeader& request_header, InferResponse* response,
    const std::shared_ptr<LabelProvider>& label_provider,
    const std::unordered_map<std::string, std::shared_ptr<SystemMemory>>&
        output_shm_buffer,
    std::shared_ptr<GRPCInferResponseProvider>* infer_provider)
{
  GRPCInferResponseProvider* provider = new GRPCInferResponseProvider(
      request_header, response, label_provider, output_shm_buffer);
  infer_provider->reset(provider);

  return Status::Success;
}

const InferResponseHeader&
GRPCInferResponseProvider::ResponseHeader() const
{
  return response_->meta_data();
}

InferResponseHeader*
GRPCInferResponseProvider::MutableResponseHeader()
{
  return response_->mutable_meta_data();
}

Status
GRPCInferResponseProvider::AllocateOutputBuffer(
    const std::string& name, void** content, size_t content_byte_size,
    const std::vector<int64_t>& content_shape)
{
  Output* output;
  RETURN_IF_ERROR(CheckAndSetIfBufferedOutput(
      name, content, content_byte_size, content_shape, &output));

  // Must always add a raw output into the list so that the number and
  // order of raw output entries equals the output meta-data. But
  // leave empty if not returning raw result for the output.
  std::string* raw_output = response_->add_raw_output();
  if (output->ptr_ == nullptr) {
    raw_output->resize(content_byte_size);
    *content = static_cast<void*>(&((*raw_output)[0]));
    output->ptr_ = *content;
  }

  return Status::Success;
}

//
// HTTPInferResponseProvider
//
HTTPInferResponseProvider::HTTPInferResponseProvider(
    evbuffer* output_buffer, const InferRequestHeader& request_header,
    const std::shared_ptr<LabelProvider>& label_provider,
    const std::unordered_map<std::string, std::shared_ptr<SystemMemory>>&
        output_shm_buffer)
    : InferResponseProvider(request_header, label_provider, output_shm_buffer),
      output_buffer_(output_buffer)
{
}

Status
HTTPInferResponseProvider::Create(
    evbuffer* output_buffer, const InferenceBackend& is,
    const InferRequestHeader& request_header,
    const std::shared_ptr<LabelProvider>& label_provider,
    const std::unordered_map<std::string, std::shared_ptr<SystemMemory>>&
        output_shm_buffer,
    std::shared_ptr<HTTPInferResponseProvider>* infer_provider)
{
  HTTPInferResponseProvider* provider = new HTTPInferResponseProvider(
      output_buffer, request_header, label_provider, output_shm_buffer);
  infer_provider->reset(provider);

  return Status::Success;
}

const InferResponseHeader&
HTTPInferResponseProvider::ResponseHeader() const
{
  return response_header_;
}

InferResponseHeader*
HTTPInferResponseProvider::MutableResponseHeader()
{
  return &response_header_;
}

Status
HTTPInferResponseProvider::AllocateOutputBuffer(
    const std::string& name, void** content, size_t content_byte_size,
    const std::vector<int64_t>& content_shape)
{
  *content = nullptr;

  Output* output;
  RETURN_IF_ERROR(CheckAndSetIfBufferedOutput(
      name, content, content_byte_size, content_shape, &output));

  if ((output->ptr_ == nullptr) && (content_byte_size > 0)) {
    // Reserve requested space in evbuffer...
    struct evbuffer_iovec output_iovec;
    if (evbuffer_reserve_space(
            output_buffer_, content_byte_size, &output_iovec, 1) != 1) {
      return Status(
          RequestStatusCode::INTERNAL, "failed to reserve " +
                                           std::to_string(content_byte_size) +
                                           " bytes in output tensor buffer");
    }

    if (output_iovec.iov_len < content_byte_size) {
      return Status(
          RequestStatusCode::INTERNAL,
          "reserved " + std::to_string(output_iovec.iov_len) +
              " bytes in output tensor buffer, need " +
              std::to_string(content_byte_size));
    }

    output_iovec.iov_len = content_byte_size;
    *content = output_iovec.iov_base;
    output->ptr_ = *content;

    // Immediately commit the buffer space. Some backends will write
    // async to the just allocated buffer space so we are relying on
    // evbuffer not to relocate this space. Because we request a
    // contiguous chunk every time (above by allowing only a single
    // entry in output_iovec), this seems to be a valid assumption.
    if (evbuffer_commit_space(output_buffer_, &output_iovec, 1) != 0) {
      *content = nullptr;
      output->ptr_ = nullptr;
      return Status(
          RequestStatusCode::INTERNAL,
          "failed to commit output tensors to output buffer");
    }
  }

  return Status::Success;
}

//
// DelegatingInferResponseProvider
//
Status
DelegatingInferResponseProvider::Create(
    const InferRequestHeader& request_header,
    const std::shared_ptr<LabelProvider>& label_provider,
    const std::unordered_map<std::string, std::shared_ptr<SystemMemory>>&
        output_shm_buffer,
    TRTSERVER_ResponseAllocator* allocator,
    TRTSERVER_ResponseAllocatorAllocFn_t alloc_fn, void* alloc_userp,
    TRTSERVER_ResponseAllocatorReleaseFn_t release_fn,
    std::shared_ptr<DelegatingInferResponseProvider>* infer_provider)
{
  DelegatingInferResponseProvider* provider =
      new DelegatingInferResponseProvider(
          request_header, label_provider, output_shm_buffer, allocator,
          alloc_fn, alloc_userp, release_fn);
  infer_provider->reset(provider);

  return Status::Success;
}

DelegatingInferResponseProvider::~DelegatingInferResponseProvider()
{
  for (const auto& output : outputs_) {
    if (output.release_buffer_ != nullptr) {
      TRTSERVER_Error* err = release_fn_(
          allocator_, output.release_buffer_, output.release_userp_,
          output.byte_size_, TRTSERVER_MEMORY_CPU, 0);
      if (err != nullptr) {
        LOG_ERROR << "failed to release result tensor '" << output.name_
                  << "': " << TRTSERVER_ErrorMessage(err);
        TRTSERVER_ErrorDelete(err);
      }
    }
  }
}

const InferResponseHeader&
DelegatingInferResponseProvider::ResponseHeader() const
{
  return response_header_;
}

InferResponseHeader*
DelegatingInferResponseProvider::MutableResponseHeader()
{
  return &response_header_;
}

Status
DelegatingInferResponseProvider::AllocateOutputBuffer(
    const std::string& name, void** content, size_t content_byte_size,
    const std::vector<int64_t>& content_shape)
{
  *content = nullptr;

  Output* output;
  RETURN_IF_ERROR(CheckAndSetIfBufferedOutput(
      name, content, content_byte_size, content_shape, &output));

  // If CheckAndSetIfBufferedOutput allocated a buffer then no
  // additional buffer is needed here but still need to call the
  // alloc_fn_ with byte-size == 0 since that is what the API
  // requires.
  const size_t alloc_byte_size = (*content != nullptr) ? 0 : content_byte_size;

  void* buffer = nullptr;
  void* buffer_userp = nullptr;

  TRTSERVER_Error* err = alloc_fn_(
      allocator_, &buffer, &buffer_userp, name.c_str(), alloc_byte_size,
      TRTSERVER_MEMORY_CPU, 0 /* region_id */, alloc_userp_);
  if (err != nullptr) {
    Status status = Status(
        TrtServerCodeToRequestStatus(TRTSERVER_ErrorCode(err)),
        TRTSERVER_ErrorMessage(err));
    TRTSERVER_ErrorDelete(err);
    return status;
  }

  // If the requested allocation size is zero then don't need to get a
  // buffer back from the allocator.
  if ((alloc_byte_size > 0) && (buffer == nullptr)) {
    return Status(
        RequestStatusCode::UNAVAILABLE,
        "unable to allocate memory for result tensor '" + name + "'");
  }

  if (*content == nullptr) {
    *content = buffer;
    output->ptr_ = buffer;
    // [TODO] actually set it after output may be allocated on non-CPU
    // https://github.com/NVIDIA/tensorrt-inference-server/pull/559
    output->memory_type_ = TRTSERVER_MEMORY_CPU;
  }

  output->release_buffer_ = buffer;
  output->release_userp_ = buffer_userp;

  return Status::Success;
}

}}  // namespace nvidia::inferenceserver
