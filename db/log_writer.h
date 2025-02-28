// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_LOG_WRITER_H_
#define STORAGE_LEVELDB_DB_LOG_WRITER_H_

#include <cstdint>

#include "db/log_format.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class WritableFile;

namespace log {

// 源码注释
// LevelDB在修改时, 首先会将修改写入到保存在文件系统上的Log, 以避免掉电时保存在内存中的数据丢失
// 由于Log是顺序写入的, 其写入速度较快。因为Log的写入是在真正执行操作之前的, 因此这一技术也叫做Write-Ahead Log

// LevelDB的Log是由Record和一些为了对齐而填充的gap组成的文件
// LevelDB在读取Log文件时, 为了减少I/O次数, 每次读取都会读入一个32KB大小的块(Block)
// 因此, 在写入Log文件时, LevelDB也将数据按照32KB对齐

// LevelDB中记录的长度是不确定的, 如果想要与32KB块对齐且为了尽可能地利用空间
// 那么较长的记录可能会被拆分为多个段, 以能够将其放入块的剩余空间中
// LevelDB定义只有1个段的记录(fragment)为FullType类型, 由多个段组成的记录(fragment)的首位段和最后端分别为FirstType与LastType, 中间段为MiddleType
// 每个段的格式如下:
//
// |header(CRC:4byte+Len:2byte+Type:1byte)|data|

// Block和Record的格式如下:
//
// [---------block1-----------][------------block2----------][---------block3----------000000]
// |--FullType--|--FirstType--|--MiddleType--|--MiddleType--|--LastType--|--FullType--|
// |fragment1   |fragment2                                               |fragment3   |

// 如果在写入时, 与32KB对齐的剩余空间不足以放入7字节的header时, LevelDB会将剩余空间填充为0x00, 并从下一个与32KB对齐(Block)处继续写入

class Writer {
 public:
  // Create a writer that will append data to "*dest".
  // "*dest" must be initially empty.
  // "*dest" must remain live while this Writer is in use.
  explicit Writer(WritableFile* dest);

  // Create a writer that will append data to "*dest".
  // "*dest" must have initial length "dest_length".
  // "*dest" must remain live while this Writer is in use.
  Writer(WritableFile* dest, uint64_t dest_length);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  ~Writer();

  Status AddRecord(const Slice& slice);

 private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_;
  int block_offset_;  // Current offset in block

  // crc32c values for all supported record types.  These are
  // pre-computed to reduce the overhead of computing the crc of the
  // record type stored in the header.
  uint32_t type_crc_[kMaxRecordType + 1];
};

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_WRITER_H_
