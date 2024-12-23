// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/merger.h"

#include "leveldb/comparator.h"
#include "leveldb/iterator.h"
#include "table/iterator_wrapper.h"

namespace leveldb {

// 源码注释
// 通过MerginIterator遍历所有iterator的key时, MergingIterator会比较其中所有iterator的key, 并按照顺序选取最小的遍历
// 在所有iterator的空间中seek时, MergingIterator会调用所有iterator的Seek方法, 然后比较所有iterator的seek结果, 按顺序选取最小的返回
// LevelDB中主要有两处使用了MergingIterator:
// 其一是用来访问整个LevelDB中数据的迭代器InternalIterator
// 该迭代器组合了MemTable Iterator、Immutable MemTable Iterator、每个Level-0 SSTable的Iterator
// 和level > 1的所有SSTable的Concatenating Iterator
// 其二是执行Major Compaction时访问需要Compact的所有SSTable的迭代器InputIterator
// 对于level-0的SSTable, 其直接组装了所有SSTable的Table Iterator, 因为level-0中每个SSTable的key空间不保证全局有序
// 而对于其它level的SSTable, 其通过Concatenating Iterator(即组装了LevelFileNumIterator和Table Iterator的TwoLevelIterator)
// 该Concatenating Iterator中组装了该层需要参与Major Compaction的SSTable
//
// [entry][entry][...][entry][entry]
//   <-kReverse       kForward->

namespace {
class MergingIterator : public Iterator {
 public:
  MergingIterator(const Comparator* comparator, Iterator** children, int n)
      : comparator_(comparator),
        children_(new IteratorWrapper[n]),
        n_(n),
        current_(nullptr),
        direction_(kForward) {
    for (int i = 0; i < n; i++) {
      children_[i].Set(children[i]);
    }
  }

  ~MergingIterator() override { delete[] children_; }

  bool Valid() const override { return (current_ != nullptr); }

  void SeekToFirst() override {
    for (int i = 0; i < n_; i++) {
      children_[i].SeekToFirst();
    }
    FindSmallest();
    direction_ = kForward;
  }

  void SeekToLast() override {
    for (int i = 0; i < n_; i++) {
      children_[i].SeekToLast();
    }
    FindLargest();
    direction_ = kReverse;
  }

  void Seek(const Slice& target) override {
    for (int i = 0; i < n_; i++) {
      children_[i].Seek(target);
    }
    FindSmallest();
    direction_ = kForward;
  }

  // 源码注释
  // 在Next移动时, 如果当前direction不是kForward的, 也就是上一次调用了Prev或者SeekToLast函数
  // 就需要先调整除current之外的所有iterator
  void Next() override {
    assert(Valid());
    // 源码注释
    // 确保所有的子Iterator都定位在key()之后, 即所有子Iterator的key()都大于等于当前iterator的key()
    // 如果我们在正向移动kForward, 对于除current_外的所有子Iterator这点已然成立
    // 因为current_是最小的子Iterator, 并且key() = current_->key()
    // 否则我们需要明确调整其它的子Iterator

    // Ensure that all children are positioned after key().
    // If we are moving in the forward direction, it is already
    // true for all of the non-current_ children since current_ is
    // the smallest child and key() == current_->key().  Otherwise,
    // we explicitly position the non-current_ children.
    if (direction_ != kForward) {
      for (int i = 0; i < n_; i++) {
        IteratorWrapper* child = &children_[i];
        if (child != current_) {
          // 把所有current之外的Iterator定位到key()之后
          child->Seek(key());
          if (child->Valid() &&
              comparator_->Compare(key(), child->key()) == 0) {
            // key等于current_->key()的再向后移动一位
            child->Next();
          }
        }
      }
      direction_ = kForward;
    }

    // current也向后移一位, 然后再查找key最小的Iterator
    current_->Next();
    FindSmallest();
  }

  // 源码注释
  // 如果考虑方向切换, 当我们调用Prev的时候, 如果之前的操作是是Next的状态, 即kForward的状态:
  // 之前Next()的状态时, 对比当前current项, 其它iterator上的值都为在current之后的值, 即基本都比currnet大(因为找最接近的所以当前的current为最小的), 或者是非Valid状态需要重新获取
  // 如果要找Prev的话, 其它iterator的项都是不满足的(因为Prev后找最接近的, 则current肯定是其他iterator中最大的),
  // 即候选项candidate应该是小于current项的值, 需要重新找候选项candidate, 如何来找呢:
  // 一种是使用iterator重新Seek到current, Seek到的值Prev一次就可以找到第一个小于它的项
  // 找到其它iterator候选项candidate后, current项可以prev一次找到上一项作为候选项candidate
  // 这样下来, 对于Prev来说, 目前所有iterator都是比之前current小的值, 取最大的就是最接近的就可以满足要求了
  // Next的实现同理
  void Prev() override {
    assert(Valid());
    // 源码注释
    // 确保所有的子Iterator都定位在key()之前, 即所有子Iterator的key()都小于等于当前iterator的key()
    // 如果我们在逆向移动, 对于除current_外的所有子Iterator这点已然成立
    // 因为current_是最大的, 并且key() = current_->key()
    // 否则我们需要明确调整其它的子Iterator

    // Ensure that all children are positioned before key().
    // If we are moving in the reverse direction, it is already
    // true for all of the non-current_ children since current_ is
    // the largest child and key() == current_->key().  Otherwise,
    // we explicitly position the non-current_ children.
    if (direction_ != kReverse) {
      for (int i = 0; i < n_; i++) {
        IteratorWrapper* child = &children_[i];
        if (child != current_) {
          child->Seek(key());
          if (child->Valid()) {
            // Child is at first entry >= key().  Step back one to be < key()
            // child位于>=key()的第一个entry上, prev移动一位到<key()
            child->Prev();
          } else {
            // Child has no entries >= key().  Position at last entry.
            // child所有的entry都 < key(), 直接seek到last即可
            child->SeekToLast();
          }
        }
      }
      direction_ = kReverse;
    }

    //current也向前移一位, 然后再查找key最大的Iterator
    current_->Prev();
    FindLargest();
  }

  Slice key() const override {
    assert(Valid());
    return current_->key();
  }

  Slice value() const override {
    assert(Valid());
    return current_->value();
  }

  Status status() const override {
    Status status;
    for (int i = 0; i < n_; i++) {
      status = children_[i].status();
      if (!status.ok()) {
        break;
      }
    }
    return status;
  }

 private:
  // Which direction is the iterator moving?
  enum Direction { kForward, kReverse };

  void FindSmallest();
  void FindLargest();

  // We might want to use a heap in case there are lots of children.
  // For now we use a simple array since we expect a very small number
  // of children in leveldb.
  const Comparator* comparator_;
  IteratorWrapper* children_;
  int n_;
  IteratorWrapper* current_;
  Direction direction_;
};

void MergingIterator::FindSmallest() {
  IteratorWrapper* smallest = nullptr;
  for (int i = 0; i < n_; i++) {
    IteratorWrapper* child = &children_[i];
    if (child->Valid()) {
      if (smallest == nullptr) {
        smallest = child;
      } else if (comparator_->Compare(child->key(), smallest->key()) < 0) {
        smallest = child;
      }
    }
  }
  current_ = smallest;
}

void MergingIterator::FindLargest() {
  IteratorWrapper* largest = nullptr;
  for (int i = n_ - 1; i >= 0; i--) {
    IteratorWrapper* child = &children_[i];
    if (child->Valid()) {
      if (largest == nullptr) {
        largest = child;
      } else if (comparator_->Compare(child->key(), largest->key()) > 0) {
        largest = child;
      }
    }
  }
  current_ = largest;
}
}  // namespace

Iterator* NewMergingIterator(const Comparator* comparator, Iterator** children,
                             int n) {
  assert(n >= 0);
  if (n == 0) {
    return NewEmptyIterator();
  } else if (n == 1) {
    return children[0];
  } else {
    return new MergingIterator(comparator, children, n);
  }
}

}  // namespace leveldb
