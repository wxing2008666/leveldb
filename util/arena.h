// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_ARENA_H_
#define STORAGE_LEVELDB_UTIL_ARENA_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace leveldb {

// 源码注释
// Arena是leveldb实现的简单的内存池(内存管理), 以最小4096bytes为单位申请block,
// 使用指针记录当前block中空余内存起始位置以及当前block剩余空间。
// 将所有的block放到blocks_数组中。
// Arena提供了分配内存以及分配对齐的内存的两种接口,
// 没有释放内存的接口, 当Arena的生命周期结束时, 由Arena 的析构函数统一释放内存。

// 用到Arena的只有两个地方:
// 1, MemTable::Add里使用Arena::Allocate分配代插入记录的内存
// 2, SkipList::NewNode里使用Arena::AllocateAligned分配SkipList::Node的内存
// MemTable::Add用于往MemTable中插入记录, 这条记录的内存即使没对齐也没关系,
// 因为不会对这块不会像遍历数组那样挨个访问, 只是开辟一块内存把东西写进去,
// 然后基本就不会访问这块内存了。若强行使用AllocateAligned只会徒增内存碎片。
// 而SkipList::Node就不一样了, SkipList::Node里有个next_[]数组，next_[]会被频繁读取。
// 如果next_[]里某个元素不是对齐的, 每次取这个元素的时候CPU都需要取两次内存, 并且会增加Cache False-Sharing的概率
class Arena {
 public:
  Arena();

  // 源码注释
  // Arena的拷贝构造函数和赋值操作符被删除, 禁止拷贝和赋值
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  ~Arena();

  // Return a pointer to a newly allocated memory block of "bytes" bytes.
  // 源码注释
  // 分配内存, 等同于malloc
  char* Allocate(size_t bytes);

  // Allocate memory with the normal alignment guarantees provided by malloc.
  // 源码注释
  // 分配对齐的内存, 等同于内存对齐的Allocate
  char* AllocateAligned(size_t bytes);

  // Returns an estimate of the total memory usage of data allocated
  // by the arena.
  size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  // Allocation state
  // 源码注释
  // 当前使用的block中空余内存起始位置以及当前block剩余空间
  char* alloc_ptr_;
  size_t alloc_bytes_remaining_;

  // Array of new[] allocated memory blocks
  // 源码注释
  // blocks_数组中保存了Arena申请的所有block
  // 每次从操作系统中申请一个block, 将其添加到blocks_数组中
  std::vector<char*> blocks_;

  // Total memory usage of the arena.
  //
  // TODO(costan): This member is accessed via atomics, but the others are
  //               accessed without any locking. Is this OK?
  std::atomic<size_t> memory_usage_;
};

inline char* Arena::Allocate(size_t bytes) {
  // The semantics of what to return are a bit messy if we allow
  // 0-byte allocations, so we disallow them here (we don't need
  // them for our internal use).
  assert(bytes > 0);
  // 源码注释
  // 如果申请的bytes小于当前block剩余的bytes, 则直接从当前block中分配内存
  // 并更新对应的指针位置以及剩余空间
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  // 源码注释
  // 如果申请的bytes大于当前block剩余的bytes, 则重新申请一个block使用
  // 并将新申请block添加到blocks_数组中, 当前使用的block为新申请的block
  return AllocateFallback(bytes);
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_ARENA_H_
