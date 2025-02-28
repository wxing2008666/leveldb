// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "leveldb/filter_policy.h"
#include "util/coding.h"

namespace leveldb {

// See doc/table_format.md for an explanation of the filter block format.

// Generate new filter every 2KB of data
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  // block_offset指对应的Data Block的偏移量
  // 用这个偏移量计算出当前Data Block对应的Filter Block的索引位置filter_index
  // 每个Filter的大小是固定的, kFilterBase默认为 2KB
  // 当给定block_offset的时候, 需要创建的filter的数目也就确定了
  uint64_t filter_index = (block_offset / kFilterBase);
  assert(filter_index >= filter_offsets_.size());
  // filter_offsets_中记录了每个Filter的起始偏移量
  // 换句话说filter_offsets_.size()就是已经构造好的Filter数量
  // 在记录新的Key之前, 需要先把积攒的Key(属于上一个Data Block的Key)都构造成Filter
  // 这里的while是上一个Data Block构建Filter, 然后清空filter buffer, 为新的Data Block做准备
  // 这里的while理解起来会有点误解, 看上去好像可能会为了1个Data Block构建多个Filter的样子, 其实不是
  // 假设Data Block大小为4KB, Filter大小为 2KB, 那么Filter 0对应Data Block 0, Filter 1是个空壳,不对应任何 Data Block
  // 假设Data Block 的大小为 1KB, Filter 的大小为2KB, 那么就会有1个Filter对应2个Data Block

  // 当已经生成的filter的数目小于需要生成的filter的总数时, 那么就继续创建filter
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}

// Filter Block的格式:
// +-----------------------------------+
// |          Bloom Filter 1           |
// +-----------------------------------+
// |          Bloom Filter 2           |
// +-----------------------------------+
// |               ...                 |
// +-----------------------------------+
// |          Bloom Filter N           |
// +-----------------------------------+
// |   Offset of Bloom Filter 1 (4B)   |
// +-----------------------------------+
// |   Offset of Bloom Filter 2 (4B)   |
// +-----------------------------------+
// |               ...                 |
// +-----------------------------------+
// |   Offset of Bloom Filter N (4B)   |
// +-----------------------------------+
// |   Offset Array Start (4 bytes)    |
// +-----------------------------------+
// |   lg(Base) (1 byte)               |
// +-----------------------------------+

Slice FilterBlockBuilder::Finish() {
  // 如果还有key, 那么把剩下的key用来生成filter
  if (!start_.empty()) {
    GenerateFilter();
  }

  // Append array of per-filter offsets
  const uint32_t array_offset = result_.size();
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }

  PutFixed32(&result_, array_offset);
  result_.push_back(kFilterBaseLg);  // Save encoding parameter in result
  return Slice(result_);
}

// 根据filter buffer生成一个filter, 将该filter压入到result_中, 并更新 filter_offsets
// 如果filter buffer里没有任何Key, 那么就只是往filter_offsets_里压入一个位置
// 这个位置指向上一个filter的位置, 不往result_里压入任何 filter
void FilterBlockBuilder::GenerateFilter() {
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    // 如果key数目为0, 这里应该是表示要新生成一个filter
    // 这时应该是重新记录下offset了
    filter_offsets_.push_back(result_.size());
    return;
  }

  // Make list of keys from flattened key structure
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i + 1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  filter_offsets_.push_back(result_.size());
  policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0) {
  size_t n = contents.size();
  if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
  base_lg_ = contents[n - 1];
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  if (last_word > n - 5) return;
  data_ = contents.data();
  offset_ = data_ + last_word;
  num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    uint32_t start = DecodeFixed32(offset_ + index * 4);
    // 当前这个filter的limit肯定是下一个filter的开头位置
    uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      Slice filter = Slice(data_ + start, limit - start);
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      // Empty filters do not match any keys
      // 如果这个filter是空的, 那么直接返回不存在
      // 多半的原因是这段内存里面没有key
      return false;
    }
  }
  return true;  // Errors are treated as potential matches
}

}  // namespace leveldb
