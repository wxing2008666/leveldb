// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_H_

#include <cstddef>
#include <cstdint>

#include "leveldb/iterator.h"

namespace leveldb {

struct BlockContents;
class Comparator;

// 源码注释
// Block的整体结构如下, 分为三部分:
//
// |---block data---|---type---|---crc---|
//
// block data: 存储了key-value数据, 并且有序排列
// type: 存储了block data的压缩类型, 1个字节
// crc: 存储了block data的crc校验码, 4个字节; 1-byte type + 32-bit crc = kBlockTrailerSize
//
// block data部分又可以分为以下几个部分:
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
// +------------+----------------+-----------+---------------------+-----------------------+
// | shared_len | not_shared_len | value_len | not_shared_key_data(key delta) | value_data |
// +------------+----------------+-----------+---------------------+-----------------------+
//
// shared_len: 共享前缀长度, 即 和前一个key相同前缀的长度
// not_shared_len: 不共享前缀长度, 即 和前一个 key不同的后缀部分的长度
// value_len: value的长度
// not_shared_key_data: 不共享前缀数据, 即 和前一个key不同的后缀部分的数据(key delta)
// value_data: value的数据


class Block {
 public:
  // Initialize the block with the specified contents.
  explicit Block(const BlockContents& contents);

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  ~Block();

  size_t size() const { return size_; }
  Iterator* NewIterator(const Comparator* comparator);

 private:
  class Iter;

  uint32_t NumRestarts() const;

  const char* data_;
  size_t size_;
  uint32_t restart_offset_;  // Offset in data_ of restart array
  bool owned_;               // Block owns data_[]
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_H_
