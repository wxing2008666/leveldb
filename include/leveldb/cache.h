// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A Cache is an interface that maps keys to values.  It has internal
// synchronization and may be safely accessed concurrently from
// multiple threads.  It may automatically evict entries to make room
// for new entries.  Values have a specified charge against the cache
// capacity.  For example, a cache where the values are variable
// length strings, may use the length of the string as the charge for
// the string.
//
// A builtin cache implementation with a least-recently-used eviction
// policy is provided.  Clients may use their own implementations if
// they want something more sophisticated (like scan-resistance, a
// custom eviction policy, variable cache sizing, etc.)

#ifndef STORAGE_LEVELDB_INCLUDE_CACHE_H_
#define STORAGE_LEVELDB_INCLUDE_CACHE_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/slice.h"

// 源码注释
// 为了减少热点数据访问时磁盘I/O频繁导致的效率问题, LevelDB在访问SSTable时加入了缓存
// LevelDB源码中, Cache是一个接口类, 用于映射key到value。
// 它具有内部同步机制, 可以安全地从多个线程并发访问。它还能够自动逐出旧缓存项以为新缓存项腾出空间
// 根据功能可以分为两种Cache:
// 1, BlockCache: 缓存最近使用的SSTable中DataBlock数据
// 2, TableCache: TableCache可以认为是一个双层Cache。其第一层Cache缓存最近打开的SSTable中的部分元数据（如索引等）;
// 而第二层Cache即为BlockCache, 缓存了当前SSTable中的DataBlock数据。TableCache提供的Get接口会同时查询两层缓存
// 无论是BlockCache还是TableCache, 其内部的核心实现都是分片的LRU缓存（Least-Recently-Used）

// LevelDB在实现BlockCache与TableCache时, 都用到了ShardedLRUCache。
// BlockCache直接使用了ShardedLRUCache, TableCache则对ShardedLRUCache又进行了一次封装。二者的主要区别在于key/value的类型及cache的大小：

// BlockCache: 用户可通过Options.block_cache配置来自定义BlockCache的实现, 其默认实现为8MB的ShardedLRUCache。
// 其key/value为(table.cache_id,block.offset)->(Block*)。
// TableCache: 用户可通过OptionTable.max_open_file配置来自定义TableCache的大小, 其默认可以保存1000个Table的信息。
// 其key/value为(SSTable.file_number)->(TableAndFile*)。


namespace leveldb {

class LEVELDB_EXPORT Cache;

// Create a new cache with a fixed size capacity.  This implementation
// of Cache uses a least-recently-used eviction policy.
LEVELDB_EXPORT Cache* NewLRUCache(size_t capacity);

class LEVELDB_EXPORT Cache {
 public:
  Cache() = default;

  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  // Destroys all existing entries by calling the "deleter"
  // function that was passed to the constructor.
  virtual ~Cache();

  // Opaque handle to an entry stored in the cache.
  // 源码注释
  // 从Cache用户的视角来看, 该Handle用来指向Cache中的一个缓存项, 即Handle是用户访问Cache中缓存项的一个凭证
  struct Handle {};

  // Insert a mapping from key->value into the cache and assign it
  // the specified charge against the total cache capacity.
  //
  // Returns a handle that corresponds to the mapping.  The caller
  // must call this->Release(handle) when the returned mapping is no
  // longer needed.
  //
  // When the inserted entry is no longer needed, the key and
  // value will be passed to "deleter".
  // 源码注释
  // 该接口用于向Cache中插入一个缓存项, 其参数key/value为待插入的键值对; charge为该缓存项所占用的容量
  // Insert方法是Cache用户将key/value插入为Cache缓存项的方法, 其参数key是Slice引用类型, value是任意类型指针,
  // size_t用来告知Cache该缓存项占用容量。显然, Cache不需要知道value具体占用多大空间, 也无从得知其类型
  // 这说明Cache的用户需要自己控制value的空间释放。
  // Insert方法的最后一个参数回调函数*deleter即用来释放value空间的方法（LevelDB内部实现的Cache会深拷贝key的数据, 不需要用户释放）。
  // 为了避免释放仍在使用的缓存项, 同时提供线程安全地访问, 缓存项的释放需要依赖引用计数。
  // 当用户更新了key相同的缓存或删除key相应的缓存时, Cache只会将其移出其管理结构, 不会释放其内存空间。
  // 只有当其引用计数归零时才会通过之前传入的回调函数deleter释放。用户对缓存项引用计数的操作即通过Handle来实现。
  // 用户在通过Insert或LookUp方法得到缓存项的Handle时, 缓存项的引用计数会+1。
  // 用户在不需要继续使用该缓存项时, 需要调用Release方法并传入该缓存项的Handle。Release方法会使缓存项的引用计数-1。
  virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                         void (*deleter)(const Slice& key, void* value)) = 0;

  // If the cache has no mapping for "key", returns nullptr.
  //
  // Else return a handle that corresponds to the mapping.  The caller
  // must call this->Release(handle) when the returned mapping is no
  // longer needed.
  virtual Handle* Lookup(const Slice& key) = 0;

  // Release a mapping returned by a previous Lookup().
  // REQUIRES: handle must not have been released yet.
  // REQUIRES: handle must have been returned by a method on *this.
  virtual void Release(Handle* handle) = 0;

  // Return the value encapsulated in a handle returned by a
  // successful Lookup().
  // REQUIRES: handle must not have been released yet.
  // REQUIRES: handle must have been returned by a method on *this.
  virtual void* Value(Handle* handle) = 0;

  // If the cache contains entry for key, erase it.  Note that the
  // underlying entry will be kept around until all existing handles
  // to it have been released.
  virtual void Erase(const Slice& key) = 0;

  // Return a new numeric id.  May be used by multiple clients who are
  // sharing the same cache to partition the key space.  Typically the
  // client will allocate a new id at startup and prepend the id to
  // its cache keys.
  virtual uint64_t NewId() = 0;

  // Remove all cache entries that are not actively in use.  Memory-constrained
  // applications may wish to call this method to reduce memory usage.
  // Default implementation of Prune() does nothing.  Subclasses are strongly
  // encouraged to override the default implementation.  A future release of
  // leveldb may change Prune() to a pure abstract method.
  virtual void Prune() {}

  // Return an estimate of the combined charges of all elements stored in the
  // cache.
  virtual size_t TotalCharge() const = 0;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_CACHE_H_
