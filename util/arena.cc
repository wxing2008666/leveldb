// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/arena.h"

namespace leveldb {

static const int kBlockSize = 4096;

Arena::Arena()
    : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {}

// 源码注释
// 析构时, 把所有new出来的内存delete掉
// 注意使用delete[], 因为new的时候使用的是new char[]
Arena::~Arena() {
  for (size_t i = 0; i < blocks_.size(); i++) {
    delete[] blocks_[i];
  }
}

char* Arena::AllocateFallback(size_t bytes) {
  // 源码注释
  // 当分配的内存大于块的1/4时, 直接从操作系统分配, 不从当前block中分配
  // 以此保证将每个block的内存碎片限制在1/4以内, 避免每个block的内存碎片过多
  // 从OS申请一个bytes大小的block, 这个block不再用于后续分配内存, 用户单独享用
  if (bytes > kBlockSize / 4) {
    // Object is more than a quarter of our block size.  Allocate it separately
    // to avoid wasting too much space in leftover bytes.
    char* result = AllocateNewBlock(bytes);
    return result;
  }

  // We waste the remaining space in the current block.
  // 源码注释
  // 只有bytes小于当前block剩余的bytes时才会调用AllocateFallback
  // 所以此时肯定要向OS申请一个新的block, 并把新申请的block作为当前使用的block
  // 上一个block的内存碎片浪费掉
  // 将kBlockSize设为4K, 有以下几点好处:
  // 减少内存碎片: Linux的内存管理里,每次内存申请都以页为单位,页的大小是4KB。
  // 比如说从OS拿5KB的内存, 操作系统实际上会分配2页的内存也就是8KB, 这就会产生3KB的内存碎片。
  // 而如果每次申请的内存都是4KB的整数倍, OS就会刚好分配1页, 不会产生内存碎片。
  // 减少Page Fault的开销：4KB意味着这段内存空间位于一张页面上, 只需做1次Page Fault。
  // 更好利用CPU缓存: 1个cache-line的大小是64B, 4KB刚好是64B的整数倍, 连续的数据块更有可能完全位于单个cache-line内。
  // 降低Cache False-Sharing的概率
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateAligned(size_t bytes) {
  // 源码注释
  // 计算对齐的长度, 最小对齐长度为8。
  // 如果当前平台的字长大于8, 则对齐长度为字长。
  // 确保align是2的幂次方
  // x & (x-1) 是个位运算的技巧, 用于快速的将x的最低一位1置为0。
  // 比如x = 0b1011, 则x & (x-1) = 0b1010。
  // 此处用(align & (align - 1)) == 0)快速判断align是否为2的幂。
  // 因为2的幂的二进制表示总是只有一位为1, 所以x & (x-1) == 0。
  const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
  static_assert((align & (align - 1)) == 0,
                "Pointer size should be a power of 2");
  // 源码注释
  // 位运算技巧, 等同于 current_mod = alloc_ptr_ % align
  // 为了对齐, 多分配slop个字节
  size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
  size_t slop = (current_mod == 0 ? 0 : align - current_mod);
  size_t needed = bytes + slop;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // AllocateFallback always returned aligned memory
    // 源码注释
    // AllocateFallback本身就是对齐的, 所以直接调用即可。
    // 因为AllocateFallback要么从os直接分配,
    // 要么new一个4KB的block, 返回block的首地址
    result = AllocateFallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  char* result = new char[block_bytes];
  blocks_.push_back(result);
  memory_usage_.fetch_add(block_bytes + sizeof(char*),
                          std::memory_order_relaxed);
  return result;

  // 源码注释
  // 为什么更新memory_usage_和读取memory_usage_使用的都是std::memory_order_relaxed呢？
  // 因为memory_usage_的上下文里没有需要读取memory_usage_的地方, 不需要对指令重排做约束。
}

}  // namespace leveldb
