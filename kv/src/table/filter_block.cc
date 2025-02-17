// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "kv/filter_policy.h"
#include "util/coding.h"

namespace kv {

// See doc/table_format.md for an explanation of the filter block format.

// Generate new filter every 2KB of data
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {
}

// 这里默认对每隔 2KB 的数据，产生一个 filter。
void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  uint64_t filter_index = (block_offset / kFilterBase);
  assert(filter_index >= filter_offsets_.size());
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}

// 返回一个完整的 filter block
Slice FilterBlockBuilder::Finish() {
// [filter 0]
// [filter 1]
// [filter 2]
// ...
// [filter N-1]

// [offset of filter 0]                  : 4 bytes
// [offset of filter 1]                  : 4 bytes
// [offset of filter 2]                  : 4 bytes
// ...
// [offset of filter N-1]                : 4 bytes

// [offset of beginning of offset array] : 4 bytes
// lg(base)                              : 1 byte 
  if (!start_.empty()) {
    GenerateFilter();
  }

  // Append array of per-filter offsets
  // filter 已经放到 result_ 里面了，现在放 offset 
  // fprintf(stdout, "filter number: %d\n", filter_offsets_.size());
  const uint32_t array_offset = result_.size();
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }

  // [offset of beginning of offset array] : 4 bytes
  PutFixed32(&result_, array_offset);
  
  // lg(base) 
  // 相当于存了11，也就是16进制的 0x0b
  result_.push_back(kFilterBaseLg);  // Save encoding parameter in result
  return Slice(result_);
}

void FilterBlockBuilder::GenerateFilter() {
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    filter_offsets_.push_back(result_.size());
    return;
  }

  // Make list of keys from flattened key structure
  // 这里是用于对下面 size_t length = start_[i+1] - start_[i]; 使用的。
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  // 将keys_里面序列化的数据，存成 Slice 数组
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i+1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  filter_offsets_.push_back(result_.size());
  // 这里将创建的 filter 数据 append 到 result_ string 后面。
  policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy),
      data_(nullptr),
      offset_(nullptr),
      num_(0),
      base_lg_(0) {
  size_t n = contents.size();
  if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
  base_lg_ = contents[n-1];
  // last_word 就是 offset of beginning of offset array，不能大于 n - 5. 
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  if (last_word > n - 5) return;

  data_ = contents.data();
  // offset_ 指向 [offset of filter 0]
  offset_ = data_ + last_word;
  // num_ 多少个 filter
  num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    // filter 的开始
    uint32_t start = DecodeFixed32(offset_ + index*4);
    // filter 的结束
    uint32_t limit = DecodeFixed32(offset_ + index*4 + 4);
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      // 恢复 filter 为一个 slice
      Slice filter = Slice(data_ + start, limit - start);
      // 使用对应的 policy 去检验 key 是不是 在 filter 里面。
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      // Empty filters do not match any keys
      return false;
    }
  }
  return true;  // Errors are treated as potential matches
}

}
