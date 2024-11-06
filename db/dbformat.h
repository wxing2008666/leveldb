// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_DBFORMAT_H_
#define STORAGE_LEVELDB_DB_DBFORMAT_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "leveldb/comparator.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/slice.h"
#include "leveldb/table_builder.h"
#include "util/coding.h"
#include "util/logging.h"

namespace leveldb {

// Grouping of constants.  We may want to make some of these
// parameters set via options.
namespace config {
static const int kNumLevels = 7;

// Level-0 compaction is started when we hit this many files.
static const int kL0_CompactionTrigger = 4;

// Soft limit on number of level-0 files.  We slow down writes at this point.
static const int kL0_SlowdownWritesTrigger = 8;

// Maximum number of level-0 files.  We stop writes at this point.
static const int kL0_StopWritesTrigger = 12;

// Maximum level to which a new compacted memtable is pushed if it
// does not create overlap.  We try to push to level 2 to avoid the
// relatively expensive level 0=>1 compactions and to avoid some
// expensive manifest file operations.  We do not push all the way to
// the largest level since that can generate a lot of wasted disk
// space if the same key space is being repeatedly overwritten.
static const int kMaxMemCompactLevel = 2;

// Approximate gap in bytes between samples of data read during iteration.
static const int kReadBytesPeriod = 1048576;

}  // namespace config

class InternalKey;

// Value types encoded as the last component of internal keys.
// DO NOT CHANGE THESE ENUM VALUES: they are embedded in the on-disk
// data structures.
enum ValueType { kTypeDeletion = 0x0, kTypeValue = 0x1 };
// kValueTypeForSeek defines the ValueType that should be passed when
// constructing a ParsedInternalKey object for seeking to a particular
// sequence number (since we sort sequence numbers in decreasing order
// and the value type is embedded as the low 8 bits in the sequence
// number in internal keys, we need to use the highest-numbered
// ValueType, not the lowest).
static const ValueType kValueTypeForSeek = kTypeValue;

typedef uint64_t SequenceNumber;

// We leave eight bits empty at the bottom so a type and sequence#
// can be packed together into 64-bits.
// 源码注释
// 64位, 最高8位为0, 8位用于存储类型标识符, 56位存储sequence number
static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

// 源码注释
// Key的设计需要保存用户所存入的User Key信息
// 另一方面还必须存在一个序号来表示同一个User Key的多个版本更新
// InnoDB存储引擎为了实现MVCC则是将一个全局递增的Transaciton ID写入到B+Tree聚簇索引的行记录中
// 而leveldb则是使用一个全局递增的序列号SequenceNumber写入到Key中
// 以实现Snapshot功能, 本质上就是MVCC
// 从另一个角度来说, 如果某个DB支持MVCC或者说快照读功能的话,那么在其内部一定存在一个全局递增的序号
// 并且该序号是必须和用户数据一起被持久化至硬盘中的
// 最后, 当我们使用Delete删除一个Key时, 实际上并不会找到这条数据并物理删除
// 而是追加写一条带有删除标志位的Key
// 所以我们还需要一个标志位, 来表示当前Key是否被删除, leveldb中使用ValueType这个枚举类实现
// 实际上, User Key、SequenceNumber以及ValueType正是组成一个Key的必要组件
// 并且在这些组件之上还会有一些额外的扩展, 这些扩展也只是简单地使用Varint来记录User Key的长度

// InternalKey本质上就是一个字符串, 由 User Key、SequenceNumber以及ValueType组成, 是一个组合结构
// ParsedInternalKey其实就是对InternalKey的解析
// 将 User Key、SequenceNumber以及ValueType从InternalKey中提取出来并保存起来

//
// ParsedInternalKey -> | User Key | SequenceNumber | ValueType |
//
// InternalKey -> | User Key | (SequenceNumber << 8|ValueType) |

// leveldb将 User Key、SequenceNumber以及ValueType拼接成InternalKey时并不是简单的Append
// 而是将ValueType揉到了SequenceNumber的低8位中以节省存储空间

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence;
  ValueType type;

  ParsedInternalKey() {}  // Intentionally left uninitialized (for speed)
  ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t)
      : user_key(u), sequence(seq), type(t) {}
  std::string DebugString() const;
};

// Return the length of the encoding of "key".
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}

// Append the serialization of "key" to *result.
// 源码注释
// 将ParsedInternalKey中的三个组件打包成InternalKey并存放到result中
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// Attempt to parse an internal key from "internal_key".  On success,
// stores the parsed data in "*result", and returns true.
//
// On error, returns false, leaves "*result" in an undefined state.
// 源码注释
// 将InternalKey拆解成三个组件并扔到result的相应字段中
bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result);

// Returns the user key portion of an internal key.
inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return Slice(internal_key.data(), internal_key.size() - 8);
}

// A comparator for internal keys that uses a specified comparator for
// the user key portion and breaks ties by decreasing sequence number.
class InternalKeyComparator : public Comparator {
 private:
  const Comparator* user_comparator_;

 public:
  explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) {}
  const char* Name() const override;
  int Compare(const Slice& a, const Slice& b) const override;
  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override;
  void FindShortSuccessor(std::string* key) const override;

  const Comparator* user_comparator() const { return user_comparator_; }

  int Compare(const InternalKey& a, const InternalKey& b) const;
};

// Filter policy wrapper that converts from internal keys to user keys
class InternalFilterPolicy : public FilterPolicy {
 private:
  const FilterPolicy* const user_policy_;

 public:
  explicit InternalFilterPolicy(const FilterPolicy* p) : user_policy_(p) {}
  const char* Name() const override;
  void CreateFilter(const Slice* keys, int n, std::string* dst) const override;
  bool KeyMayMatch(const Slice& key, const Slice& filter) const override;
};

// Modules in this directory should keep internal keys wrapped inside
// the following class instead of plain strings so that we do not
// incorrectly use string comparisons instead of an InternalKeyComparator.
class InternalKey {
 private:
  std::string rep_;

 public:
  InternalKey() {}  // Leave rep_ as empty to indicate it is invalid
  InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
  }

  bool DecodeFrom(const Slice& s) {
    rep_.assign(s.data(), s.size());
    return !rep_.empty();
  }

  Slice Encode() const {
    assert(!rep_.empty());
    return rep_;
  }

  Slice user_key() const { return ExtractUserKey(rep_); }

  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(&rep_, p);
  }

  void Clear() { rep_.clear(); }

  std::string DebugString() const;
};

inline int InternalKeyComparator::Compare(const InternalKey& a,
                                          const InternalKey& b) const {
  return Compare(a.Encode(), b.Encode());
}

inline bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  uint8_t c = num & 0xff;
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  result->user_key = Slice(internal_key.data(), n - 8);
  return (c <= static_cast<uint8_t>(kTypeValue));
}

// A helper class useful for DBImpl::Get()
// 源码注释
// 当我们查询一个User Key时, 其查询顺序为MemTable、Immutable Table以及位于硬盘中的SSTable
// MemTable所提供的Get()方法需要使用到LookupKey,
// 从该对象中我们可以得到所有我们需要的信息包括User Key、User Key的长度、Sequence Number以及Value Type;
// LookupKey其实就是在InternalKey的基础上额外的添加了User Key的长度,这个长度是由Varint进行编码的
// 程序为了能够正确的找到User Key和SequenceNumber等信息, 额外的使用了3个指针
//
// LookupKey -> |Size(Varint)|User Key|(SequenceNumber << 8|ValueType)|
//              ^            ^                                        ^
//              start_       kstart_                                  end_
//
// Size的大小为User Key的字节数再加上8, 然后通过EncodeVarint32()方法进行编码写入到字符串的头部
// 对于LookupKey来说, 其Value Type为kValueTypeForSeek其实也就是kTypeValue
// MemTableKey与LookupKey:
//
// MemTable Entry -> |Key Size(Varint, user_key.size()+8)|User Key|(SequenceNumber << 8|ValueType)|Vaule Size(Varint)|User Value|
//
// leveldb使用SkipList来实现位于内存中的MemTable并提供了Add()方法将Key-Value写入至SkipList中
// 在SkipList的实现中,我们并没有发现Value字段,这是因为leveldb将User Key和User Value打包成一个更大的Key
// 直接塞到了SkipList中, 具体实现可见MemTable::Add() 方法
class LookupKey {
 public:
  // Initialize *this for looking up user_key at a snapshot with
  // the specified sequence number.
  LookupKey(const Slice& user_key, SequenceNumber sequence);

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  ~LookupKey();

  // Return a key suitable for lookup in a MemTable.
  // 源码注释
  // 可以看到MemTable Key和LookupKey其实是等价的
  Slice memtable_key() const { return Slice(start_, end_ - start_); }

  // Return an internal key (suitable for passing to an internal iterator)
  Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }

  // Return the user key
  Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }

 private:
  // We construct a char array of the form:
  //    klength  varint32               <-- start_
  //    userkey  char[klength]          <-- kstart_
  //    tag      uint64
  //                                    <-- end_
  // The array is a suitable MemTable key.
  // The suffix starting with "userkey" can be used as an InternalKey.
  const char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];  // Avoid allocation for short keys
};

inline LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_DBFORMAT_H_
