// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_

#include <cstdint>
#include <vector>

#include "leveldb/slice.h"

namespace leveldb {

struct Options;

// 源码注释
// BlockBuilder是一个Block的构建器, 它会将Block的Key-Value以及重启点信息编码到字符串buffer_中
// BlockBuilder数据格式:
//
// +-------------------------+
// |   entry(Key1:Value1)   |
// +-------------------------+
// |   entry(Key2:Value2)   |
// +-------------------------+
// |           ...          |
// +-------------------------+
// |   entry(KeyN:ValueN)   |
// +-------------------------+
// |      restart point1    |
// +-------------------------+
// |     restart point2     |
// +-------------------------+
// |          ...           |
// +-------------------------+
// |     restart point M    |
// +-------------------------+
// |     num of restarts    |
// +-------------------------+
//
// 每条entry的数据格式:
// +------------+----------------+-----------+---------------------+------------+
// | shared_len | not_shared_len | value_len | not_shared_key_data | value_data |
// +------------+----------------+-----------+---------------------+------------+



class BlockBuilder {
 public:
  explicit BlockBuilder(const Options* options);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // Reset the contents as if the BlockBuilder was just constructed.
  // 恢复到一个空白 Block 的状态
  void Reset();

  // REQUIRES: Finish() has not been called since the last call to Reset().
  // REQUIRES: key is larger than any previously added key
  void Add(const Slice& key, const Slice& value);

  // Finish building the block and return a slice that refers to the
  // block contents.  The returned slice will remain valid for the
  // lifetime of this builder or until Reset() is called.
  // 将重启点信息也压入到Block缓冲区里, 结束该Block的构建, 然后返回 buffer_
  Slice Finish();

  // Returns an estimate of the current (uncompressed) size of the block
  // we are building.
  size_t CurrentSizeEstimate() const;

  // Return true iff no entries have been added since the last Reset()
  bool empty() const { return buffer_.empty(); }

 private:
  const Options* options_;
  std::string buffer_;              // Destination buffer
  std::vector<uint32_t> restarts_;  // Restart points
  int counter_;                     // Number of entries emitted since restart
  bool finished_;                   // Has Finish() been called?
  std::string last_key_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
