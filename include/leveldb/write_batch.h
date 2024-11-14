// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch holds a collection of updates to apply atomically to a DB.
//
// The updates are applied in the order in which they are added
// to the WriteBatch.  For example, the value of "key" will be "v3"
// after the following batch is written:
//
//    batch.Put("key", "v1");
//    batch.Delete("key");
//    batch.Put("key", "v2");
//    batch.Put("key", "v3");
//
// Multiple threads can invoke const methods on a WriteBatch without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same WriteBatch must use
// external synchronization.

#ifndef STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
#define STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_

#include <string>

#include "leveldb/export.h"
#include "leveldb/status.h"

namespace leveldb {

class Slice;

// 源码注释
// 当调用db->Put(WriteOptions(), &key, &value)写入数据时, 如果WriteOptions只有一个变量sync,且默认初始值为false
// 则leveldb默认的写数据方式是异步, 即每将写操作提交将数据写入到内存中就返回, 而同步写是将数据从内存写到磁盘成功后返回
// 异步写比同步写的效率高得多, 但是当系统故障时可能导致最近的更新丢失
// 若将WriteOptions的sync设为true, 则每次写入都会将数据写入到磁盘中速度非常慢
// 为此leveldb使用WriteBatch来替代简单的异步写操作, 首先将所有的写操作记录到一个batch中
// 然后执行同步写, 这样同步写的开销就被分散到多个写操作中
// WriteBatch的原理是先将所有的操作记录下来, 然后再一起操作, 即批量操作多条记录

// 每一个WriteBatch都是以一个固定长度的头部开始, 然后后面接着多条连续的记录
// 头部固定12字节, 其中前8字节为WriteBatch的序列号, 对应rep_[0]到rep_[7]
// 每次处理Batch中的记录时才会更新, 后四字节为当前Batch中的记录数, 对应rep_[8]到rep_[11]
// 随后的记录结构为:
// 插入数据时: kTypeValue, [Key_size, Key], [Value_size, Value]
// 删除数据时: kTypeDeletion, [Key_size, Key]

// Handler声明在WriteBatch内, 也说明Handler类只用于WriteBatch(从属关系上属于)
// 友元类WriteBatchInternal其实是IMPL设计模式的一种变种, 这里就是为了将不相关的功能性函数和实际的业务数据之间进行分离
// 同时又能提供兼容性, 提高类的可复用性; WriteBatchInternal其实就是将WriteBatch中和具体业务数据不相关的功能函数进行抽离
// 通过这种方法能够有效的降低代码的耦合性，还能提高代码的可阅读性

class LEVELDB_EXPORT WriteBatch {
 public:
  class LEVELDB_EXPORT Handler {
   public:
    virtual ~Handler();
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };

  WriteBatch();

  // Intentionally copyable.
  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;

  ~WriteBatch();

  // Store the mapping "key->value" in the database.
  void Put(const Slice& key, const Slice& value);

  // If the database contains a mapping for "key", erase it.  Else do nothing.
  void Delete(const Slice& key);

  // Clear all updates buffered in this batch.
  void Clear();

  // The size of the database changes caused by this batch.
  //
  // This number is tied to implementation details, and may change across
  // releases. It is intended for LevelDB usage metrics.
  size_t ApproximateSize() const;

  // Copies the operations in "source" to this batch.
  //
  // This runs in O(source size) time. However, the constant factor is better
  // than calling Iterate() over the source batch with a Handler that replicates
  // the operations into this batch.
  void Append(const WriteBatch& source);

  // Support for iterating over the contents of a batch.
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  std::string rep_;  // See comment in write_batch.cc for the format of rep_
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
