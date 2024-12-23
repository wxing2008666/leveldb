// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/two_level_iterator.h"

#include "leveldb/table.h"
#include "table/block.h"
#include "table/format.h"
#include "table/iterator_wrapper.h"

namespace leveldb {

namespace {

// 源码注释
// TwoLevelIterator在LevelDB中常用于有索引结构的二级查询
// 可以将index的iterator和data的iterator组合到一起
// seek时TwoLevelIterator会先通过index iterator, seek到相应的index处
// 并将index的value作为arg传给data iterator(block_funciton)通过data iterator来访问真正的数据
// 如果从key顺序的角度来看TwoLevelIterator, 其需要index有序、每个index下的data有序、所有index下的所有data全局有序
// 即TwoLevelIterator实际上是一个建立在多级查找结构上的iterator; LevelDB中主要有两个符合该结构的组件:

// 其一是level>0的SSTable, 其每层SSTable可以按照key排序, 每个SSTable内也按照key排序且每层SSTable中的key没有overlap且全局有序
// 因此LevelDB中Version的Concaterating Iterator实际上就是一个TwoLevelIterator
// 其第一级iterator是LevelFileNumIterator, 该iterator按照key的顺序遍历每层SSTable
// 其第二级iterator是Table Iterator, 该iterator可以按照key的顺序遍历SSTable中的key/value
// Table Iterator本身也是一个TwoLevelIterator, 这也是LevelDB中第二个符合该结构的部分

// 其二即为SSTable内部的index与data。Table Iterator作为TwoLevelIterator
// 其第一级iterator遍历SSTable中的index, 第二级iterator遍历index相应的data block中的key/value

// 如果每个iterator中的key有序, 但是所有iterator中的所有key全局无序, 就不能使用TwoLevelIterator来组装多个iterator
// 此时需要一种能够"归并"多路有序iterator的结构, 即MergingIterator

// SST格式:
/*
+---------------------+
|   Data Block 1      |
+---------------------+
|   Data Block 2      |
+---------------------+
|        ...          |
+---------------------+
|   Data Block N      |
+---------------------+
|   Meta Block 1      |
+---------------------+
|        ...          |
+---------------------+
|   Meta Block K      |
+---------------------+
| Meta Index Block    |
+---------------------+
|   Index Block       |
+---------------------+
|      Footer         |
+---------------------+
*/
// SST将Key-Value分散在多个Data Block里, Index Block里存储每个Data Block的Key范围和在SST文件中的偏移量
// Index Block格式:
/*
+--------------------------------------------------+
| Key1 | Block Handle1(指向第一个 Data Block 的信息) |
+--------------------------------------------------+
| Key2 | Block Handle2(指向第二个 Data Block 的信息) |
+--------------------------------------------------+
| Key3 | Block Handle3                             |
+--------------------------------------------------+
| ...............                                  |
+--------------------------------------------------+
| KeyN | Block HandleN                             |
+--------------------------------------------------+
*/
// Block Handle1里包含了Data Block1在SST文件中的偏移量和大小
// 现在要查找一个Key, 需要先到IndexBlock中通过二分查找的方式找到对应的Block Handle
// 然后通过这个Block Handle找到对应的Data Block, 最后再到Data Block中查找这个Key


typedef Iterator* (*BlockFunction)(void*, const ReadOptions&, const Slice&);

class TwoLevelIterator : public Iterator {
 public:
  TwoLevelIterator(Iterator* index_iter, BlockFunction block_function,
                   void* arg, const ReadOptions& options);

  ~TwoLevelIterator() override;

  void Seek(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;
  void Next() override;
  void Prev() override;

  bool Valid() const override { return data_iter_.Valid(); }
  Slice key() const override {
    assert(Valid());
    return data_iter_.key();
  }
  Slice value() const override {
    assert(Valid());
    return data_iter_.value();
  }
  Status status() const override {
    // It'd be nice if status() returned a const Status& instead of a Status
    if (!index_iter_.status().ok()) {
      return index_iter_.status();
    } else if (data_iter_.iter() != nullptr && !data_iter_.status().ok()) {
      return data_iter_.status();
    } else {
      return status_;
    }
  }

 private:
  void SaveError(const Status& s) {
    if (status_.ok() && !s.ok()) status_ = s;
  }
  void SkipEmptyDataBlocksForward();
  void SkipEmptyDataBlocksBackward();
  void SetDataIterator(Iterator* data_iter);
  void InitDataBlock();

  BlockFunction block_function_;
  void* arg_;
  const ReadOptions options_;
  Status status_;
  IteratorWrapper index_iter_;
  IteratorWrapper data_iter_;  // May be nullptr
  // If data_iter_ is non-null, then "data_block_handle_" holds the
  // "index_value" passed to block_function_ to create the data_iter_.
  std::string data_block_handle_;
};

TwoLevelIterator::TwoLevelIterator(Iterator* index_iter,
                                   BlockFunction block_function, void* arg,
                                   const ReadOptions& options)
    : block_function_(block_function),
      arg_(arg),
      options_(options),
      index_iter_(index_iter),
      data_iter_(nullptr) {}

TwoLevelIterator::~TwoLevelIterator() = default;

void TwoLevelIterator::Seek(const Slice& target) {
  index_iter_.Seek(target);
  InitDataBlock();
  if (data_iter_.iter() != nullptr) data_iter_.Seek(target);
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToFirst() {
  index_iter_.SeekToFirst();
  InitDataBlock();
  if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToLast() {
  index_iter_.SeekToLast();
  InitDataBlock();
  if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::Next() {
  assert(Valid());
  data_iter_.Next();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::Prev() {
  assert(Valid());
  data_iter_.Prev();
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::SkipEmptyDataBlocksForward() {
  while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
    // Move to next block
    if (!index_iter_.Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    index_iter_.Next();
    InitDataBlock();
    if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
  }
}

void TwoLevelIterator::SkipEmptyDataBlocksBackward() {
  while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
    // Move to next block
    if (!index_iter_.Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    index_iter_.Prev();
    InitDataBlock();
    if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
  }
}

void TwoLevelIterator::SetDataIterator(Iterator* data_iter) {
  if (data_iter_.iter() != nullptr) SaveError(data_iter_.status());
  data_iter_.Set(data_iter);
}

void TwoLevelIterator::InitDataBlock() {
  if (!index_iter_.Valid()) {
    SetDataIterator(nullptr);
  } else {
    Slice handle = index_iter_.value();
    if (data_iter_.iter() != nullptr &&
        handle.compare(data_block_handle_) == 0) {
      // data_iter_ is already constructed with this iterator, so
      // no need to change anything
    } else {
      Iterator* iter = (*block_function_)(arg_, options_, handle);
      data_block_handle_.assign(handle.data(), handle.size());
      SetDataIterator(iter);
    }
  }
}

}  // namespace

Iterator* NewTwoLevelIterator(Iterator* index_iter,
                              BlockFunction block_function, void* arg,
                              const ReadOptions& options) {
  return new TwoLevelIterator(index_iter, block_function, arg, options);
}

}  // namespace leveldb
