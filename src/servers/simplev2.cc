// Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
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

#include <unistd.h>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "src/core/tritonserver.h"
#include "src/servers/common.h"

#ifdef TRTIS_ENABLE_GPU
#include <cuda_runtime_api.h>
#endif  // TRTIS_ENABLE_GPU

namespace ni = nvidia::inferenceserver;

namespace {

bool enforce_memory_type = false;
TRITONSERVER_MemoryType requested_memory_type;

#ifdef TRTIS_ENABLE_GPU
static auto cuda_data_deleter = [](void* data) {
  if (data != nullptr) {
    cudaPointerAttributes attr;
    auto cuerr = cudaPointerGetAttributes(&attr, data);
    if (cuerr != cudaSuccess) {
      std::cerr << "error: failed to get CUDA pointer attribute of " << data
                << ": " << cudaGetErrorString(cuerr) << std::endl;
    }
    if (attr.type == cudaMemoryTypeDevice) {
      cuerr = cudaFree(data);
    } else if (attr.type == cudaMemoryTypeHost) {
      cuerr = cudaFreeHost(data);
    }
    if (cuerr != cudaSuccess) {
      std::cerr << "error: failed to release CUDA pointer " << data << ": "
                << cudaGetErrorString(cuerr) << std::endl;
    }
  }
};
#endif  // TRTIS_ENABLE_GPU

void
Usage(char** argv, const std::string& msg = std::string())
{
  if (!msg.empty()) {
    std::cerr << msg << std::endl;
  }

  std::cerr << "Usage: " << argv[0] << " [options]" << std::endl;
  std::cerr << "\t-m <\"system\"|\"pinned\"|gpu>"
            << " Enforce the memory type for input and output tensors."
            << " If not specified, inputs will be in system memory and outputs"
            << " will be based on the model's preferred type." << std::endl;
  std::cerr << "\t-v Enable verbose logging" << std::endl;
  std::cerr << "\t-r [model repository absolute path]" << std::endl;

  exit(1);
}

TRITONSERVER_Error*
ResponseAlloc(
    TRITONSERVER_ResponseAllocator* allocator, const char* tensor_name,
    size_t byte_size, TRITONSERVER_MemoryType preferred_memory_type,
    int64_t preferred_memory_type_id, void* userp, void** buffer,
    void** buffer_userp, TRITONSERVER_MemoryType* actual_memory_type,
    int64_t* actual_memory_type_id)
{
  // Pass the tensor name with buffer_userp so we can show it when
  // releasing the buffer.

  // Unless necessary, the actual memory type and id is the same as preferred
  // memory type
  *actual_memory_type = preferred_memory_type;
  *actual_memory_type_id = 0;

  // If 'byte_size' is zero just return 'buffer'==nullptr, we don't
  // need to do any other book-keeping.
  if (byte_size == 0) {
    *buffer = nullptr;
    *buffer_userp = nullptr;
    std::cout << "allocated " << byte_size << " bytes for result tensor "
              << tensor_name << std::endl;
  } else {
    void* allocated_ptr = nullptr;
    if (enforce_memory_type) {
      *actual_memory_type = requested_memory_type;
    }
    switch (*actual_memory_type) {
#ifdef TRTIS_ENABLE_GPU
      case TRITONSERVER_MEMORY_CPU_PINNED: {
        auto err = cudaSetDevice(*actual_memory_type_id);
        if ((err != cudaSuccess) && (err != cudaErrorNoDevice) &&
            (err != cudaErrorInsufficientDriver)) {
          return TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INTERNAL,
              std::string(
                  "unable to recover current CUDA device: " +
                  std::string(cudaGetErrorString(err)))
                  .c_str());
        }

        err = cudaHostAlloc(&allocated_ptr, byte_size, cudaHostAllocPortable);
        if (err != cudaSuccess) {
          return TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INTERNAL,
              std::string(
                  "cudaHostAlloc failed: " +
                  std::string(cudaGetErrorString(err)))
                  .c_str());
        }
        break;
      }
      case TRITONSERVER_MEMORY_GPU: {
        auto err = cudaSetDevice(*actual_memory_type_id);
        if ((err != cudaSuccess) && (err != cudaErrorNoDevice) &&
            (err != cudaErrorInsufficientDriver)) {
          return TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INTERNAL,
              std::string(
                  "unable to recover current CUDA device: " +
                  std::string(cudaGetErrorString(err)))
                  .c_str());
        }

        err = cudaMalloc(&allocated_ptr, byte_size);
        if (err != cudaSuccess) {
          return TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INTERNAL,
              std::string(
                  "cudaMalloc failed: " + std::string(cudaGetErrorString(err)))
                  .c_str());
        }
        break;
      }
#endif  // TRTIS_ENABLE_GPU
      // Fallback if unknown type.
      case TRITONSERVER_MEMORY_CPU:
      default: {
        *actual_memory_type = TRITONSERVER_MEMORY_CPU;
        allocated_ptr = malloc(byte_size);
        break;
      }
    }

    if (allocated_ptr != nullptr) {
      *buffer = allocated_ptr;
      *buffer_userp = new std::string(tensor_name);
      std::cout << "allocated " << byte_size << " bytes in "
                << ni::MemoryTypeString(*actual_memory_type)
                << " for result tensor " << tensor_name << std::endl;
    }
  }

  return nullptr;  // Success
}

TRITONSERVER_Error*
ResponseRelease(
    TRITONSERVER_ResponseAllocator* allocator, void* buffer, void* buffer_userp,
    size_t byte_size, TRITONSERVER_MemoryType memory_type,
    int64_t memory_type_id)
{
  std::string* name = nullptr;
  if (buffer_userp != nullptr) {
    name = reinterpret_cast<std::string*>(buffer_userp);
  } else {
    name = new std::string("<unknown>");
  }

  std::cout << "Releasing buffer " << buffer << " of size " << byte_size
            << " in " << ni::MemoryTypeString(memory_type) << " for result '"
            << *name << "'" << std::endl;
  switch (memory_type) {
    case TRITONSERVER_MEMORY_CPU:
      free(buffer);
      break;
#ifdef TRTIS_ENABLE_GPU
    case TRITONSERVER_MEMORY_CPU_PINNED: {
      auto err = cudaSetDevice(memory_type_id);
      if (err == cudaSuccess) {
        err = cudaFreeHost(buffer);
      }
      if (err != cudaSuccess) {
        std::cerr << "error: failed to cudaFree " << buffer << ": "
                  << cudaGetErrorString(err) << std::endl;
      }
      break;
    }
    case TRITONSERVER_MEMORY_GPU: {
      auto err = cudaSetDevice(memory_type_id);
      if (err == cudaSuccess) {
        err = cudaFree(buffer);
      }
      if (err != cudaSuccess) {
        std::cerr << "error: failed to cudaFree " << buffer << ": "
                  << cudaGetErrorString(err) << std::endl;
      }
      break;
    }
#endif  // TRTIS_ENABLE_GPU
    default:
      std::cerr << "error: unexpected buffer allocated in CUDA managed memory"
                << std::endl;
      break;
  }

  delete name;

  return nullptr;  // Success
}

void
InferRequestComplete(TRITONSERVER_InferenceRequest* request, void* userp)
{
  // We reuse the request so we don't delete it here...
}

void
InferResponseComplete(TRITONSERVER_InferenceResponse* response, void* userp)
{
  // Send 'response' to the future.
  std::promise<TRITONSERVER_InferenceResponse*>* p =
      reinterpret_cast<std::promise<TRITONSERVER_InferenceResponse*>*>(userp);
  p->set_value(response);
  delete p;
}

TRITONSERVER_Error*
ParseModelMetadata(
    const rapidjson::Document& model_metadata, bool* is_int,
    bool* is_torch_model)
{
  std::string seen_data_type;
  for (const auto& input : model_metadata["inputs"].GetArray()) {
    if (strcmp(input["datatype"].GetString(), "INT32") &&
        strcmp(input["datatype"].GetString(), "FP32")) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_UNSUPPORTED,
          "simple lib example only supports model with data type INT32 or "
          "FP32");
    }
    if (seen_data_type.empty()) {
      seen_data_type = input["datatype"].GetString();
    } else if (strcmp(seen_data_type.c_str(), input["datatype"].GetString())) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INVALID_ARG,
          "the inputs and outputs of 'simple' model must have the data type");
    }
  }
  for (const auto& output : model_metadata["outputs"].GetArray()) {
    if (strcmp(output["datatype"].GetString(), "INT32") &&
        strcmp(output["datatype"].GetString(), "FP32")) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_UNSUPPORTED,
          "simple lib example only supports model with data type INT32 or "
          "FP32");
    } else if (strcmp(seen_data_type.c_str(), output["datatype"].GetString())) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INVALID_ARG,
          "the inputs and outputs of 'simple' model must have the data type");
    }
  }

  *is_int = (strcmp(seen_data_type.c_str(), "INT32") == 0);
  *is_torch_model =
      (strcmp(model_metadata["platform"].GetString(), "pytorch_libtorch") == 0);
  return nullptr;
}

template <typename T>
void
GenerateInputData(
    std::vector<char>* input0_data, std::vector<char>* input1_data)
{
  input0_data->resize(16 * sizeof(T));
  input1_data->resize(16 * sizeof(T));
  for (size_t i = 0; i < 16; ++i) {
    ((T*)input0_data->data())[i] = i;
    ((T*)input1_data->data())[i] = 1;
  }
}

template <typename T>
void
CompareResult(
    const std::string& output0_name, const std::string& output1_name,
    const void* input0, const void* input1, const char* output0,
    const char* output1)
{
  for (size_t i = 0; i < 16; ++i) {
    std::cout << ((T*)input0)[i] << " + " << ((T*)input1)[i] << " = "
              << ((T*)output0)[i] << std::endl;
    std::cout << ((T*)input0)[i] << " - " << ((T*)input1)[i] << " = "
              << ((T*)output1)[i] << std::endl;

    if ((((T*)input0)[i] + ((T*)input1)[i]) != ((T*)output0)[i]) {
      FAIL("incorrect sum in " + output0_name);
    }
    if ((((T*)input0)[i] - ((T*)input1)[i]) != ((T*)output1)[i]) {
      FAIL("incorrect difference in " + output1_name);
    }
  }
}

void
Check(
    TRITONSERVER_InferenceResponse* response,
    const std::vector<char>& input0_data, const std::vector<char>& input1_data,
    const std::string& output0, const std::string& output1,
    const size_t expected_byte_size,
    const TRITONSERVER_DataType expected_datatype, const bool is_int)
{
  std::unordered_map<std::string, std::vector<char>> output_data;

  uint32_t output_count;
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceResponseOutputCount(response, &output_count),
      "getting number of response outputs");
  if (output_count != 2) {
    FAIL("expecting 2 response outputs, got " + std::to_string(output_count));
  }

  for (uint32_t idx = 0; idx < output_count; ++idx) {
    const char* cname;
    TRITONSERVER_DataType datatype;
    const int64_t* shape;
    uint64_t dim_count;
    const void* base;
    size_t byte_size;
    TRITONSERVER_MemoryType memory_type;
    int64_t memory_type_id;

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceResponseOutput(
            response, idx, &cname, &datatype, &shape, &dim_count, &base,
            &byte_size, &memory_type, &memory_type_id),
        "getting output info");

    if (cname == nullptr) {
      FAIL("unable to get output name");
    }

    std::string name(cname);
    if ((name != output0) && (name != output1)) {
      FAIL("unexpected output '" + name + "'");
    }

    if ((dim_count != 2) || (shape[0] != 1) || (shape[1] != 16)) {
      FAIL("unexpected shape for '" + name + "'");
    }

    if (datatype != expected_datatype) {
      FAIL(
          "unexpected datatype '" +
          std::string(TRITONSERVER_DataTypeString(datatype)) + "' for '" +
          name + "'");
    }

    if (byte_size != expected_byte_size) {
      FAIL(
          "unexpected byte-size, expected " +
          std::to_string(expected_byte_size) + ", got " +
          std::to_string(byte_size) + " for " + name);
    }

    if (enforce_memory_type && (memory_type != requested_memory_type)) {
      FAIL(
          "unexpected memory type, expected to be allocated "
          "in " +
          ni::MemoryTypeString(requested_memory_type) + ", got " +
          ni::MemoryTypeString(memory_type) + ", id " +
          std::to_string(memory_type_id) + " for " + name);
    }

    // We make a copy of the data here... which we could avoid for
    // performance reasons but ok for this simple example.
    std::vector<char>& odata = output_data[name];
    switch (memory_type) {
      case TRITONSERVER_MEMORY_CPU: {
        std::cout << name << " is stored in system memory" << std::endl;
        const char* cbase = reinterpret_cast<const char*>(base);
        odata.assign(cbase, cbase + byte_size);
        break;
      }

      case TRITONSERVER_MEMORY_CPU_PINNED: {
        std::cout << name << " is stored in pinned memory" << std::endl;
        const char* cbase = reinterpret_cast<const char*>(base);
        odata.assign(cbase, cbase + byte_size);
        break;
      }

#ifdef TRTIS_ENABLE_GPU
      case TRITONSERVER_MEMORY_GPU: {
        std::cout << name << " is stored in GPU memory" << std::endl;
        odata.reserve(byte_size);
        FAIL_IF_CUDA_ERR(
            cudaMemcpy(&odata[0], base, byte_size, cudaMemcpyDeviceToHost),
            "getting " + name + " data from GPU memory");
        break;
      }
#endif

      default:
        FAIL("unexpected memory type");
    }
  }

  if (is_int) {
    CompareResult<int32_t>(
        output0, output1, &input0_data[0], &input1_data[0],
        output_data[output0].data(), output_data[output1].data());
  } else {
    CompareResult<float>(
        output0, output1, &input0_data[0], &input1_data[0],
        output_data[output0].data(), output_data[output1].data());
  }
}

}  // namespace

int
main(int argc, char** argv)
{
  std::string model_repository_path;
  int verbose_level = 0;

  // Parse commandline...
  int opt;
  while ((opt = getopt(argc, argv, "vm:r:")) != -1) {
    switch (opt) {
      case 'm': {
        enforce_memory_type = true;
        if (!strcmp(optarg, "system")) {
          requested_memory_type = TRITONSERVER_MEMORY_CPU;
        } else if (!strcmp(optarg, "pinned")) {
          requested_memory_type = TRITONSERVER_MEMORY_CPU_PINNED;
        } else if (!strcmp(optarg, "gpu")) {
          requested_memory_type = TRITONSERVER_MEMORY_GPU;
        } else {
          Usage(
              argv,
              "-m must be used to specify one of the following types:"
              " <\"system\"|\"pinned\"|gpu>");
        }
        break;
      }
      case 'r':
        model_repository_path = optarg;
        break;
      case 'v':
        verbose_level = 1;
        break;
      case '?':
        Usage(argv);
        break;
    }
  }

  if (model_repository_path.empty()) {
    Usage(argv, "-r must be used to specify model repository path");
  }
#ifndef TRTIS_ENABLE_GPU
  if (enforce_memory_type && requested_memory_type != TRITONSERVER_MEMORY_CPU) {
    Usage(argv, "-m can only be set to \"system\" without enabling GPU");
  }
#endif  // TRTIS_ENABLE_GPU

  // Create the server...
  TRITONSERVER_ServerOptions* server_options = nullptr;
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_ServerOptionsNew(&server_options),
      "creating server options");
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_ServerOptionsSetModelRepositoryPath(
          server_options, model_repository_path.c_str()),
      "setting model repository path");
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_ServerOptionsSetLogVerbose(server_options, verbose_level),
      "setting verbose logging level");

  TRITONSERVER_Server* server_ptr = nullptr;
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_ServerNew(&server_ptr, server_options), "creating server");
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_ServerOptionsDelete(server_options),
      "deleting server options");

  std::shared_ptr<TRITONSERVER_Server> server(
      server_ptr, TRITONSERVER_ServerDelete);

  // Wait until the server is both live and ready.
  size_t health_iters = 0;
  while (true) {
    bool live, ready;
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_ServerIsLive(server.get(), &live),
        "unable to get server liveness");
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_ServerIsReady(server.get(), &ready),
        "unable to get server readiness");
    std::cout << "Server Health: live " << live << ", ready " << ready
              << std::endl;
    if (live && ready) {
      break;
    }

    if (++health_iters >= 10) {
      FAIL("failed to find healthy inference server");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  // Print status of the server.
  {
    TRITONSERVER_Message* server_metadata_message;
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_ServerMetadata(server.get(), &server_metadata_message),
        "unable to get server metadata message");
    const char* buffer;
    size_t byte_size;
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_MessageSerializeToJson(
            server_metadata_message, &buffer, &byte_size),
        "unable to serialize server metadata message");

    std::cout << "Server Status:" << std::endl;
    std::cout << std::string(buffer, byte_size) << std::endl;

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_MessageDelete(server_metadata_message),
        "deleting status metadata");
  }

  const std::string model_name("simple");

  // Wait for the model to become available.
  bool is_torch_model = false;
  bool is_int = true;
  bool is_ready = false;
  health_iters = 0;
  while (!is_ready) {
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_ServerModelIsReady(
            server.get(), model_name.c_str(), "1", &is_ready),
        "unable to get model readiness");
    if (!is_ready) {
      if (++health_iters >= 10) {
        FAIL("model failed to be ready in 10 iterations");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    TRITONSERVER_Message* model_metadata_message;
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_ServerModelMetadata(
            server.get(), model_name.c_str(), "1", &model_metadata_message),
        "unable to get model metadata message");
    const char* buffer;
    size_t byte_size;
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_MessageSerializeToJson(
            model_metadata_message, &buffer, &byte_size),
        "unable to serialize model status protobuf");

    rapidjson::Document model_metadata;
    model_metadata.Parse(buffer, byte_size);
    if (model_metadata.HasParseError()) {
      FAIL(
          "error: failed to parse model metadata from JSON: " +
          std::string(GetParseError_En(model_metadata.GetParseError())) +
          " at " + std::to_string(model_metadata.GetErrorOffset()));
    }

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_MessageDelete(model_metadata_message),
        "deleting status protobuf");

    if (strcmp(model_metadata["name"].GetString(), model_name.c_str())) {
      FAIL("unable to find metadata for model");
    }

    bool found_version = false;
    if (model_metadata.HasMember("versions")) {
      for (const auto& version : model_metadata["versions"].GetArray()) {
        if (strcmp(version.GetString(), "1") == 0) {
          found_version = true;
          break;
        }
      }
    }
    if (!found_version) {
      FAIL("unable to find version 1 status for model");
    }

    FAIL_IF_TRITON_ERR(
        ParseModelMetadata(model_metadata, &is_int, &is_torch_model),
        "parsing model metadata");
  }

  // Create the allocator that will be used to allocate buffers for
  // the result tensors.
  TRITONSERVER_ResponseAllocator* allocator = nullptr;
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_ResponseAllocatorNew(
          &allocator, ResponseAlloc, ResponseRelease),
      "creating response allocator");

  // Inference
  TRITONSERVER_InferenceRequest* irequest = nullptr;
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceRequestNew(
          &irequest, server.get(), model_name.c_str(),
          nullptr /* model_version */),
      "creating inference request");

  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceRequestSetId(irequest, "my_request_id"),
      "setting ID for the request");

  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceRequestSetReleaseCallback(
          irequest, InferRequestComplete, nullptr /* request_release_userp */),
      "setting request release callback");

  // Inputs
  auto input0 = is_torch_model ? "INPUT__0" : "INPUT0";
  auto input1 = is_torch_model ? "INPUT__1" : "INPUT1";

  std::vector<int64_t> input0_shape({1, 16});
  std::vector<int64_t> input1_shape({1, 16});

  const TRITONSERVER_DataType datatype =
      (is_int) ? TRITONSERVER_TYPE_INT32 : TRITONSERVER_TYPE_FP32;

  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceRequestAddInput(
          irequest, input0, datatype, &input0_shape[0], input0_shape.size()),
      "setting input 0 meta-data for the request");
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceRequestAddInput(
          irequest, input1, datatype, &input1_shape[0], input1_shape.size()),
      "setting input 1 meta-data for the request");

  auto output0 = is_torch_model ? "OUTPUT__0" : "OUTPUT0";
  auto output1 = is_torch_model ? "OUTPUT__1" : "OUTPUT1";
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceRequestAddRequestedOutput(irequest, output0),
      "requesting output 0 for the request");
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceRequestAddRequestedOutput(irequest, output1),
      "requesting output 1 for the request");

  // Create the data for the two input tensors. Initialize the first
  // to unique values and the second to all ones.
  std::vector<char> input0_data;
  std::vector<char> input1_data;
  if (is_int) {
    GenerateInputData<int32_t>(&input0_data, &input1_data);
  } else {
    GenerateInputData<float>(&input0_data, &input1_data);
  }

  size_t input0_size = input0_data.size();
  size_t input1_size = input1_data.size();

  const void* input0_base = &input0_data[0];
  const void* input1_base = &input1_data[0];
#ifdef TRTIS_ENABLE_GPU
  std::unique_ptr<void, decltype(cuda_data_deleter)> input0_gpu(
      nullptr, cuda_data_deleter);
  std::unique_ptr<void, decltype(cuda_data_deleter)> input1_gpu(
      nullptr, cuda_data_deleter);
  bool use_cuda_memory =
      (enforce_memory_type &&
       (requested_memory_type != TRITONSERVER_MEMORY_CPU));
  if (use_cuda_memory) {
    FAIL_IF_CUDA_ERR(cudaSetDevice(0), "setting CUDA device to device 0");
    if (requested_memory_type != TRITONSERVER_MEMORY_CPU_PINNED) {
      void* dst;
      FAIL_IF_CUDA_ERR(
          cudaMalloc(&dst, input0_size),
          "allocating GPU memory for INPUT0 data");
      input0_gpu.reset(dst);
      FAIL_IF_CUDA_ERR(
          cudaMemcpy(dst, &input0_data[0], input0_size, cudaMemcpyHostToDevice),
          "setting INPUT0 data in GPU memory");
      FAIL_IF_CUDA_ERR(
          cudaMalloc(&dst, input1_size),
          "allocating GPU memory for INPUT1 data");
      input1_gpu.reset(dst);
      FAIL_IF_CUDA_ERR(
          cudaMemcpy(dst, &input1_data[0], input1_size, cudaMemcpyHostToDevice),
          "setting INPUT1 data in GPU memory");
    } else {
      void* dst;
      FAIL_IF_CUDA_ERR(
          cudaHostAlloc(&dst, input0_size, cudaHostAllocPortable),
          "allocating pinned memory for INPUT0 data");
      input0_gpu.reset(dst);
      FAIL_IF_CUDA_ERR(
          cudaMemcpy(dst, &input0_data[0], input0_size, cudaMemcpyHostToHost),
          "setting INPUT0 data in pinned memory");
      FAIL_IF_CUDA_ERR(
          cudaHostAlloc(&dst, input1_size, cudaHostAllocPortable),
          "allocating pinned memory for INPUT1 data");
      input1_gpu.reset(dst);
      FAIL_IF_CUDA_ERR(
          cudaMemcpy(dst, &input1_data[0], input1_size, cudaMemcpyHostToHost),
          "setting INPUT1 data in pinned memory");
    }
  }

  input0_base = use_cuda_memory ? input0_gpu.get() : &input0_data[0];
  input1_base = use_cuda_memory ? input1_gpu.get() : &input1_data[0];
#endif  // TRTIS_ENABLE_GPU

  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceRequestAppendInputData(
          irequest, input0, input0_base, input0_size, requested_memory_type,
          0 /* memory_type_id */),
      "assigning INPUT0 data");
  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceRequestAppendInputData(
          irequest, input1, input1_base, input1_size, requested_memory_type,
          0 /* memory_type_id */),
      "assigning INPUT1 data");

  // Perform inference...
  {
    auto p = new std::promise<TRITONSERVER_InferenceResponse*>();
    std::future<TRITONSERVER_InferenceResponse*> completed = p->get_future();

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceRequestSetResponseCallback(
            irequest, allocator, nullptr /* response_allocator_userp */,
            InferResponseComplete, reinterpret_cast<void*>(p)),
        "setting response callback");

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_ServerInferAsync(
            server.get(), irequest, nullptr /* trace_manager */,
            nullptr /* trace_release_fn */, nullptr /* trace_release_userp */),
        "running inference");

    // Wait for the inference to complete.
    TRITONSERVER_InferenceResponse* completed_response = completed.get();

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceResponseError(completed_response),
        "response status");

    Check(
        completed_response, input0_data, input1_data, output0, output1,
        input0_size, datatype, is_int);

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceResponseDelete(completed_response),
        "deleting inference response");
  }

  // Modify some input data in place and then reuse the request
  // object. For simplicity we only do this when the input tensors are
  // in non-pinned system memory.
  if (!enforce_memory_type ||
      (requested_memory_type == TRITONSERVER_MEMORY_CPU)) {
    if (is_int) {
      int32_t* input0_base = reinterpret_cast<int32_t*>(&input0_data[0]);
      input0_base[0] = 27;
    } else {
      float* input0_base = reinterpret_cast<float*>(&input0_data[0]);
      input0_base[0] = 27.0;
    }

    auto p = new std::promise<TRITONSERVER_InferenceResponse*>();
    std::future<TRITONSERVER_InferenceResponse*> completed = p->get_future();

    // Using a new promise so have to re-register the callback to set
    // the promise as the userp.
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceRequestSetResponseCallback(
            irequest, allocator, nullptr /* response_allocator_userp */,
            InferResponseComplete, reinterpret_cast<void*>(p)),
        "setting response callback");

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_ServerInferAsync(
            server.get(), irequest, nullptr /* trace_manager */,
            nullptr /* trace_release_fn */, nullptr /* trace_release_userp */),
        "running inference");

    // Wait for the inference to complete.
    TRITONSERVER_InferenceResponse* completed_response = completed.get();
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceResponseError(completed_response),
        "response status");

    Check(
        completed_response, input0_data, input1_data, output0, output1,
        input0_size, datatype, is_int);

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceResponseDelete(completed_response),
        "deleting inference response");
  }

  // Remove input data and then add back different data.
  {
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceRequestRemoveAllInputData(irequest, input0),
        "removing INPUT0 data");
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceRequestAppendInputData(
            irequest, input0, input1_base, input1_size, requested_memory_type,
            0 /* memory_type_id */),
        "assigning INPUT1 data to INPUT0");

    auto p = new std::promise<TRITONSERVER_InferenceResponse*>();
    std::future<TRITONSERVER_InferenceResponse*> completed = p->get_future();

    // Using a new promise so have to re-register the callback to set
    // the promise as the userp.
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceRequestSetResponseCallback(
            irequest, allocator, nullptr /* response_allocator_userp */,
            InferResponseComplete, reinterpret_cast<void*>(p)),
        "setting response callback");

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_ServerInferAsync(
            server.get(), irequest, nullptr /* trace_manager */,
            nullptr /* trace_release_fn */, nullptr /* trace_release_userp */),
        "running inference");

    // Wait for the inference to complete.
    TRITONSERVER_InferenceResponse* completed_response = completed.get();
    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceResponseError(completed_response),
        "response status");

    // Both inputs are using input1_data...
    Check(
        completed_response, input1_data, input1_data, output0, output1,
        input0_size, datatype, is_int);

    FAIL_IF_TRITON_ERR(
        TRITONSERVER_InferenceResponseDelete(completed_response),
        "deleting inference response");
  }

  FAIL_IF_TRITON_ERR(
      TRITONSERVER_InferenceRequestDelete(irequest),
      "deleting inference request");

  FAIL_IF_TRITON_ERR(
      TRITONSERVER_ResponseAllocatorDelete(allocator),
      "deleting response allocator");

  return 0;
}
