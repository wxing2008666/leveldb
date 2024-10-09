// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_
#define STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_

#include <string>

#include "leveldb/export.h"

namespace leveldb {

class Slice;

// A Comparator object provides a total order across slices that are
// used as keys in an sstable or a database.  A Comparator implementation
// must be thread-safe since leveldb may invoke its methods concurrently
// from multiple threads.
// 源码注释
// LevelDB中将Key之间的比较抽象为Comparator, 方便用户根据不同的业务场景实现不同的比较策略
// LevelDB没有规定比较的规则, 只是定义了一个Comparator接口, 用户可以提供自己的规则实现这个接口
// 默认使用BytewiseComparatorImpl
class LEVELDB_EXPORT Comparator {
 public:
  virtual ~Comparator();

  // Three-way comparison.  Returns value:
  //   < 0 iff "a" < "b",
  //   == 0 iff "a" == "b",
  //   > 0 iff "a" > "b"
  virtual int Compare(const Slice& a, const Slice& b) const = 0;

  // The name of the comparator.  Used to check for comparator
  // mismatches (i.e., a DB created with one comparator is
  // accessed using a different comparator.
  //
  // The client of this package should switch to a new name whenever
  // the comparator implementation changes in a way that will cause
  // the relative ordering of any two keys to change.
  //
  // Names starting with "leveldb." are reserved and should not be used
  // by any clients of this package.
  // 源码注释
  // Comparator的名称, 用于检查 Comparator是否匹配
  // 比如数据库创建时使用的是 Comparator A
  // 重新打开数据库时使用的是 Comparator B
  // 此时 LevelDB 则会检测到 Comparator 不匹配
  virtual const char* Name() const = 0;

  // Advanced functions: these are used to reduce the space requirements
  // for internal data structures like index blocks.

  // If *start < limit, changes *start to a short string in [start,limit).
  // Simple comparator implementations may return with *start unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  // 源码注释
  // 找到一个最短的字符串seperator, 使得 start <= seperator < limit
  // 并将结果保存在start中
  // 用于 SST 中 Data Block 的索引构建
  // 主要是为了优化SSTable里的Index Block里的索引项的长度, 使得索引更短
  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const = 0;

  // Changes *key to a short string >= *key.
  // Simple comparator implementations may return with *key unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  // 源码注释
  // 找到一个最短的字符串 successor, 使得 key <= successor
  // 并将结果保存在key中
  // 即将key更改为大于key的最短的key, 这也是为了减小索引项的长度, 不过这是优化一个SSTable里最后一个索引项的
  virtual void FindShortSuccessor(std::string* key) const = 0;
};

// Return a builtin comparator that uses lexicographic byte-wise
// ordering.  The result remains the property of this module and
// must not be deleted.
LEVELDB_EXPORT const Comparator* BytewiseComparator();

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_
