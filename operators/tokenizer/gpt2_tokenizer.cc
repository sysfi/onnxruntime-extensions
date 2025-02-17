// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Partial code comes from other Microsoft employee.

#include "gpt2_tokenizer.hpp"


KernelBpeTokenizer::KernelBpeTokenizer(const OrtApi& api, const OrtKernelInfo& info)
    : BaseKernel(api, info) {
  std::string vocab = ort_.KernelInfoGetAttribute<std::string>(&info, "vocab");
  if (vocab.empty()) {
    ORTX_CXX_API_THROW("vocabulary shouldn't be empty.", ORT_INVALID_ARGUMENT);
  }

  std::string merges = ort_.KernelInfoGetAttribute<std::string>(&info, "merges");
  if (merges.empty()) {
    ORTX_CXX_API_THROW("merges shouldn't be empty.", ORT_INVALID_ARGUMENT);
  }

  if (!TryToGetAttribute<int64_t>("padding_length", padding_length_)) {
    padding_length_ = -1;
  }

  if (padding_length_ != -1 && padding_length_ <= 0) {
    ORTX_CXX_API_THROW("padding_length should be more than 0 or equal -1", ORT_INVALID_ARGUMENT);
  }

  std::stringstream vocabu_stream(vocab);
  std::stringstream merges_stream(merges);
  bbpe_tokenizer_ = std::make_shared<VocabData>();
  bbpe_tokenizer_->Load(vocabu_stream, merges_stream, "<|endoftext|>", "<|endoftext|>");
}

std::vector<int64_t> KernelBpeTokenizer::Tokenize(const ustring& input, int64_t max_length) {
  std::vector<int64_t> res;

  if (IsEmptyUString(input)) {
    return res;
  }

  auto special_token_split_res = bbpe_tokenizer_->SplitBySpecialTokens(input);
  TokenWithRegularExp regcmp;

  for (auto& seg_id : special_token_split_res) {
    if (static_cast<int64_t>(res.size()) >= max_length) break;

    if (seg_id.second != -1) {
      res.push_back(seg_id.second);
      continue;
    }

    auto cur_input = std::move(seg_id.first);
    // Note: keep ptr to make sure the string_view is valid in the following process
    const char32_t* ptr = cur_input.c_str();
    regcmp.Set(ptr);

    while (static_cast<int64_t>(res.size()) < max_length) {
      auto [b, tok] = regcmp.GetNextToken();
      if (!b) break;

      std::string utf8_token = std::string(ustring(tok));

      byte_list_.clear();
      for (char& cp : utf8_token) {
        byte_list_.push_back(bbpe_tokenizer_->ByteEncoder()[static_cast<unsigned char>(cp)]);
      }

      bbpe_tokenizer_->bpe(byte_list_);

      for (auto p : byte_list_) {
        if (static_cast<int64_t>(res.size()) >= max_length) {
          break;
        }

        res.push_back(p);
      }
    }
  }

  return res;
}

void KernelBpeTokenizer::Compute(OrtKernelContext* context) {
  // Setup inputs
  const OrtValue* input = ort_.KernelContext_GetInput(context, 0);
  std::vector<std::string> str_input;
  GetTensorMutableDataString(api_, ort_, context, input, str_input);
  OrtTensorDimensions input_dim(ort_, input);

  std::vector<std::vector<int64_t>> tokenize_results;
  for (auto& str : str_input) {
    tokenize_results.emplace_back(Tokenize(ustring(str), padding_length_ < 0 ? INT64_MAX : padding_length_));
  }

  size_t max_length = 0;
  if (padding_length_ == -1) {
    for (auto& res : tokenize_results) {
      max_length = std::max(max_length, res.size());
    }
  } else {
    max_length = static_cast<size_t>(padding_length_);
  }

  OrtTensorDimensions output_dim = input_dim;
  output_dim.push_back(max_length);
  OrtValue* tokenize_output = ort_.KernelContext_GetOutput(context, 0, output_dim.data(), output_dim.size());
  OrtValue* attention_mask = ort_.KernelContext_GetOutput(context, 1, output_dim.data(), output_dim.size());
  auto* token = ort_.GetTensorMutableData<int64_t>(tokenize_output);
  auto* mask = ort_.GetTensorMutableData<int64_t>(attention_mask);

  int idx = 0;
  for (auto& res : tokenize_results) {
    for (int64_t id : res) {
      token[idx] = id;
      mask[idx] = 1;
      idx++;
    }

    for (size_t i = res.size(); i < max_length; i++) {
      token[idx] = 0;
      mask[idx] = 0;
      idx++;
    }
  }
}

const char* CustomOpBpeTokenizer::GetName() const {
  return "GPT2Tokenizer";
}

size_t CustomOpBpeTokenizer::GetInputTypeCount() const {
  return 1;
}

ONNXTensorElementDataType CustomOpBpeTokenizer::GetInputType(size_t /*index*/) const {
  return ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING;
}
size_t CustomOpBpeTokenizer::GetOutputTypeCount() const {
  return 2;
}

ONNXTensorElementDataType CustomOpBpeTokenizer::GetOutputType(size_t /*index*/) const {
  return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
}
