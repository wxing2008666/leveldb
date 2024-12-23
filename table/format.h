// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_FORMAT_H_
#define STORAGE_LEVELDB_TABLE_FORMAT_H_

#include <cstdint>
#include <string>

#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "leveldb/table_builder.h"

namespace leveldb {

class Block;
class RandomAccessFile;
struct ReadOptions;

// 源码注释
// BlockHandle相当于一个block的"指针", 由这个block在整块数据中的的offset(varint64)和size(varint64)组成
// 有偏移量offset和大小size即可定位到这个block在文件中的位置
// 由于采用varint64进行编码, 每个varint64最多占用10字节, 所以一个BlockHandle最多占用 20 字节
// BlockHandle是定长, 而BlockHandle编码的结果是变长的

// BlockHandle is a pointer to the extent of a file that stores a data
// block or a meta block.
class BlockHandle {
 public:
  // Maximum encoding length of a BlockHandle
  enum { kMaxEncodedLength = 10 + 10 };

  BlockHandle();

  // The offset of the block in the file.
  uint64_t offset() const { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }

  // The size of the stored block
  uint64_t size() const { return size_; }
  void set_size(uint64_t size) { size_ = size; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  uint64_t offset_;
  uint64_t size_;
};

// Footer encapsulates the fixed information stored at the tail
// end of every table file.
// 源码注释
// 在一个SSTable中文件末尾的Footer是定长的, 其他数据都被划分成一个个变长的block:
// index block、metaindex block、meta blocks、data blocks
// Footer的大小为48字节, 内容是一个8字节的magic number和两个BlockHandle——index handle和meta index handle
// index handle指向index block, meta index handle指向meta index block
// BlockHandle相当于一个block的"指针", 由这个block的offset(varint64)和size(varint64)组成。
// 由于采用varint64进行编码, 每个varint64最多占用10字节, 所以一个BlockHandle最多占用 20 字节
// BlockHandle是定长, 而BlockHandle编码的结果是变长的, 所以Footer编码的时候需要进行padding
// index block中的每条key-value指向一个data block, value比较简单直接, 就是对应的data block的BlockHandle
// key是一个大于等于当前data block中最大的key且小于下一个block中最小的 key, 这一块的逻辑可以参考FindShortestSeparator的调用和实现
// 这样做是为了减小index block的体积, 毕竟我们希望程序运行的时候, index block被尽可能cache在内存中

// Meta index block: 指向Meta Block的索引, 用于快速定位Meta Block
// Meta block: 用于快速filter某一个User Key是否在当前SSTable中, 默认为Bloom Filter
// Data block是实际的key-value数据

class Footer {
 public:
  // Encoded length of a Footer.  Note that the serialization of a
  // Footer will always occupy exactly this many bytes.  It consists
  // of two block handles and a magic number.
  enum { kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8 };

  Footer() = default;

  // The block handle for the metaindex block of the table
  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

  // The block handle for the index block of the table
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

// kTableMagicNumber was picked by running
//    echo http://code.google.com/p/leveldb/ | sha1sum
// and taking the leading 64 bits.
static const uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

// 1-byte type + 32-bit crc
static const size_t kBlockTrailerSize = 5;

struct BlockContents {
  Slice data;           // Actual contents of data
  bool cachable;        // True iff data can be cached
  bool heap_allocated;  // True iff caller should delete[] data.data()
};

// Read the block identified by "handle" from "file".  On failure
// return non-OK.  On success fill *result and return OK.
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result);

// Implementation details follow.  Clients should ignore,

inline BlockHandle::BlockHandle()
    : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_FORMAT_H_
