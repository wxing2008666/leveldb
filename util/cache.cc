// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/hash.h"
#include "util/mutexlock.h"

namespace leveldb {

Cache::~Cache() {}

namespace {

// LRU cache implementation
//
// Cache entries have an "in_cache" boolean indicating whether the cache has a
// reference on the entry.  The only ways that this can become false without the
// entry being passed to its "deleter" are via Erase(), via Insert() when
// an element with a duplicate key is inserted, or on destruction of the cache.
//
// The cache keeps two linked lists of items in the cache.  All items in the
// cache are in one list or the other, and never both.  Items still referenced
// by clients but erased from the cache are in neither list.  The lists are:
// - in-use:  contains the items currently referenced by clients, in no
//   particular order.  (This list is used for invariant checking.  If we
//   removed the check, elements that would otherwise be on this list could be
//   left as disconnected singleton lists.)
// - LRU:  contains the items not currently referenced by clients, in LRU order
// Elements are moved between these lists by the Ref() and Unref() methods,
// when they detect an element in the cache acquiring or losing its only
// external reference.

// An entry is a variable length heap-allocated structure.  Entries
// are kept in a circular doubly linked list ordered by access time.

// 源码注释
// LevelDB的Cache实现中有两个用来保存缓存项LRUHandle的链表: in-use链表和LRU链表。
// in-use链表上无序保存着在LRUCache中且正在被client使用的LRUHandle（该链表仅用来保持LRUHandle引用计数）;
// LRU链表上按照最近使用的顺序保存着当前在LRUCache中但目前没有被用户使用的LRUHandle。LRUHandle在两个链表间的切换由Ref和UnRef实现。
// 在LRUCache的实现中, 在Insert方法插入LRUHandle时, 只会从LRU链表中逐出LRUHandle, 相当于in-use链表中的LRUHandle会被LRUCache "Pin"住
// 永远都不会逐出。也就是说, 对于LRUCache中的每个LRUHandle, 其只有如下几种状态:
// 对于还没存入LRUCache的LRUHandle, 不在任一链表上。
// 当前在LRUCache中, 且正在被client使用的LRUHandle, 在in-use链表上无序保存。
// 当前在LRUCache中, 当前未被client使用的LRUHandle, 在LRU链表上按LRU顺序保存。
// 之前在LRUCache中, 但①被用户通过Erase方法从LRUCache中删除
//                  或②用户通过Insert方法更新了该key的LRUHandle,
//                  或③LRUCache被销毁时, LRUHandle既不在in-use链表上也不在LRU链表上。
// 此时，该LRUHandle在等待client通过Release方法释放引用计数以销毁。
// LRUCache为了能够快速根据key来找到相应的LRUHandle, 而不需要遍历链表, 其还组装了一个哈希表HandleTable。
// LevelDB的哈希表与哈希函数都使用了自己的实现。
// LRUCache其实已经实现了完整的LRU缓存的功能。但是LevelDB又将其封装为ShardedLRUCache, 并让ShardedLRUCache实现了Cache接口供用户使用。
// ShardedLRUCache中保存了若干个LRUCache, 并根据插入的key的哈希将其分配到相应的LRUCache中。
// 因为每个LRUCache有独立的锁, 这种方式可以减少锁的争用, 以优化并行程序的性能。

// LRUHandle是表示缓存项的结构体
// LRUHandle中有记录key(深拷贝), value(浅拷贝)及相关哈希值、引用计数、占用空间、是否仍在LRUCache中等字段。
// 其中next指针与prev指针, 用来实现LRUCache中的两个链表, 而next_hash是哈希表HandleTable为了解决哈希冲突采用拉链法的链指针。
struct LRUHandle {
  void* value;
  void (*deleter)(const Slice&, void* value);
  LRUHandle* next_hash; // 如果两个缓存项的key不一样, 但hash值相同, 那么它们会被放到一个hash桶中,next_hash就是桶里的下一个缓存项
  LRUHandle* next;
  LRUHandle* prev;
  size_t charge;  // TODO(opt): Only allow uint32_t?
  size_t key_length;
  bool in_cache;     // Whether entry is in the cache. 可能在lru_链表中, 也可能在in_use_链表中
  uint32_t refs;     // References, including cache reference, if present. 引用次数为1时表示该缓存项还在Cache中但是没有正在被客户端使用;当引用次数大于1时表示该缓存项正在被客户端使用
  uint32_t hash;     // Hash of key(); used for fast sharding and comparisons
  char key_data[1];  // Beginning of key

  Slice key() const {
    // next is only equal to this if the LRU handle is the list head of an
    // empty list. List heads never have meaningful keys.
    assert(next != this);

    return Slice(key_data, key_length);
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
// 源码注释
// HandleTable实现了一个可扩展哈希表
class HandleTable {
 public:
  HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
  ~HandleTable() { delete[] list_; }

  LRUHandle* Lookup(const Slice& key, uint32_t hash) {
    return *FindPointer(key, hash);
  }

  // 源码注释
  // 插入一个新的LRUHandle, 返回一个和这个新LRUHandle相同Key的老LRUHandle如果存在的话
  LRUHandle* Insert(LRUHandle* h) {
    // 找到key对应的LRUHandle*在Hash表中的位置
    // 如果哈希表中存在相同key的缓存项, 那么返回老的 LRUHandle*在Hash表中的位置
    // 如果哈希表中不存在相同key的缓存项, 那么返回新的LRUHandle*需要插入到Hash表中的位置
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    // 如果old存在, 就用新的LRUHandle*替换掉old
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    *ptr = h;
    if (old == nullptr) {
      // 如果old不存在, 表示哈希表中需要新插入一个LRUHandle*
      // 此时需要更新哈希表的元素个数, 如果元素个数超过了哈希表的长度
      // 则需要对哈希表进行扩容
      ++elems_;
      if (elems_ > length_) {
        // Since each cache entry is fairly large, we aim for a small
        // average linked list length (<= 1).
        Resize();
      }
    }
    return old;
  }

  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != nullptr) {
      // 如果找到了, 那么需要将该LRUHandle*从Hash表中移除
      // 并且更新哈希表的元素个数
      *ptr = result->next_hash;
      --elems_;
    }
    return result;
  }

 private:
  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  uint32_t length_;
  uint32_t elems_;
  LRUHandle** list_;

  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  // 源码注释
  // FindPointer方法是根据key与其hash查找LRUHandle的方法。
  // 如果key存在则返回其LRUHandle的指针, 如果不存在则返回指向可插入的solt的指针。
  LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }

  // 源码注释
  // Resize方法是扩展哈希表的方法。该方法会倍增solt大小, 并重新分配空间。
  // 在重新分配solt的空间后, 再对所有原有solt中的LRUHandle重哈希。最后释放旧的solt的空间。
  void Resize() {
    uint32_t new_length = 4;
    while (new_length < elems_) {
      new_length *= 2;
    }
    LRUHandle** new_list = new LRUHandle*[new_length];
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      while (h != nullptr) {
        LRUHandle* next = h->next_hash;
        uint32_t hash = h->hash;
        LRUHandle** ptr = &new_list[hash & (new_length - 1)];
        h->next_hash = *ptr;
        *ptr = h;
        h = next;
        count++;
      }
    }
    assert(elems_ == count);
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
  }
};

// A single shard of sharded cache.
class LRUCache {
 public:
  LRUCache();
  ~LRUCache();

  // Separate from constructor so caller can easily make an array of LRUCache
  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  // Like Cache methods, but with an extra "hash" parameter.
  // 源码注释
  // Insert方法的作用是插入一个新的缓存项到Cache中
  // 插入一个缓存项到 Cache 中，同时注册该缓存项的销毁回调函数。
  // key: 缓存项的 key
  // hash: key 的 hash 值, 需要客户端自己计算
  // value: 缓存数据的指针
  // charge: 缓存项的大小，需要客户端自己计算，因为缓存项里只存储了缓存数据的指针
  // deleter: 缓存项的销毁回调函数
  Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value,
                        size_t charge,
                        void (*deleter)(const Slice& key, void* value));
  Cache::Handle* Lookup(const Slice& key, uint32_t hash);
  // 源码注释
  // 将缓存项的引用次数减一
  void Release(Cache::Handle* handle);
  void Erase(const Slice& key, uint32_t hash);
  // 源码注释
  // 移除Cache中所有没有正在被使用的缓存项, 也就是引用计数为1的那些
  void Prune();
  size_t TotalCharge() const {
    MutexLock l(&mutex_);
    return usage_;
  }

 private:
  void LRU_Remove(LRUHandle* e);
  void LRU_Append(LRUHandle* list, LRUHandle* e);
  void Ref(LRUHandle* e);
  void Unref(LRUHandle* e);
  bool FinishErase(LRUHandle* e) EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Initialized before use.
  size_t capacity_;

  // mutex_ protects the following state.
  mutable port::Mutex mutex_;
  size_t usage_ GUARDED_BY(mutex_);

  // Dummy head of LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // Entries have refs==1 and in_cache==true.
  // 源码注释
  // lru_链表的Dummy Head节点
  // lru_链表中存放refs == 1 && in_cache == true的缓存项
  // lru_链表中, 最新的缓存项是头节点, 最老的是尾节点
  // lru_链表的头部的节点，是访问时间最新的节点，而越靠近尾部的节点，访问时间越早
  LRUHandle lru_ GUARDED_BY(mutex_);

  // Dummy head of in-use list.
  // Entries are in use by clients, and have refs >= 2 and in_cache==true.
  // 源码注释
  // in_use_链表的Dummy Head节点
  // in_use_链表中的缓存项是正在被客户端使用的, 它们的引用次数>=2, in_cache==true
  // in_use_链表的作用是让我们能清晰的知道哪些缓存项是正在被客户端使用的
  // 哪些是在Cache中但是没有正在被使用, 这样可以实现更精细的缓存策略
  LRUHandle in_use_ GUARDED_BY(mutex_);

  // 源码注释
  // Cache中所有缓存项的Hash表, 用于快速查找缓存项
  HandleTable table_ GUARDED_BY(mutex_);
};

LRUCache::LRUCache() : capacity_(0), usage_(0) {
  // Make empty circular linked lists.
  lru_.next = &lru_;
  lru_.prev = &lru_;
  in_use_.next = &in_use_;
  in_use_.prev = &in_use_;
}

LRUCache::~LRUCache() {
  assert(in_use_.next == &in_use_);  // Error if caller has an unreleased handle
  for (LRUHandle* e = lru_.next; e != &lru_;) {
    LRUHandle* next = e->next;
    assert(e->in_cache);
    e->in_cache = false;
    assert(e->refs == 1);  // Invariant of lru_ list.
    Unref(e);
    e = next;
  }
}

void LRUCache::Ref(LRUHandle* e) {
  if (e->refs == 1 && e->in_cache) {  // If on lru_ list, move to in_use_ list.
    LRU_Remove(e);
    LRU_Append(&in_use_, e);
  }
  e->refs++;
}

void LRUCache::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  if (e->refs == 0) {  // Deallocate.
    assert(!e->in_cache);
    (*e->deleter)(e->key(), e->value);
    free(e);
  } else if (e->in_cache && e->refs == 1) {
    // No longer in use; move to lru_ list.
    LRU_Remove(e);
    LRU_Append(&lru_, e);
  }
}

void LRUCache::LRU_Remove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}

void LRUCache::LRU_Append(LRUHandle* list, LRUHandle* e) {
  // Make "e" newest entry by inserting just before *list
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}

Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  LRUHandle* e = table_.Lookup(key, hash);
  if (e != nullptr) {
    Ref(e);
  }
  return reinterpret_cast<Cache::Handle*>(e);
}

void LRUCache::Release(Cache::Handle* handle) {
  MutexLock l(&mutex_);
  Unref(reinterpret_cast<LRUHandle*>(handle));
}

Cache::Handle* LRUCache::Insert(const Slice& key, uint32_t hash, void* value,
                                size_t charge,
                                void (*deleter)(const Slice& key,
                                                void* value)) {
  MutexLock l(&mutex_);

  // 源码注释
  // 创建一个LRUHandle节点, 用来存放缓存项的元数据
  LRUHandle* e =
      reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->in_cache = false;
  // 提前把引用计数先加一, 因为Insert结束后需要把创建出来的LRUHandle地址
  // 返回给客户端, 客户端对该LRUHandle的引用需要加一
  e->refs = 1;  // for the returned handle.
  std::memcpy(e->key_data, key.data(), key.size());

  // 源码注释
  // 如果打开数据库时配置了禁止使用Cache, 则创建出来的Cache Capacity就会是0
  if (capacity_ > 0) {
    // 这里的引用计数加一表示该LRUHandle在Cache中, 是Cache对LRUHandle的引用
    e->refs++;  // for the cache's reference.
    e->in_cache = true;
    // 把LRUHandle节点按照LRU的策略插入到in_use_链表中
    LRU_Append(&in_use_, e);
    usage_ += charge;
    // 把LRUHandle节点插入到Hash表中
    // 如果存在相同key的缓存项, 那么table_.Insert(e)会返回老的缓存项
    // 如果存在老的缓存项, 那么需要将老的缓存项从Cache中移除
    FinishErase(table_.Insert(e));
  } else {  // don't cache. (capacity_==0 is supported and turns off caching.)
    // next is read by key() in an assert, so it must be initialized
    // capacity_==0表示禁止使用Cache, 所以这里不需要把LRUHandle节点插入到链表中
    e->next = nullptr;
  }
  // 源码注释
  // 如果插入新的LRUHandle节点后, Cache的总大小超过了容量, 那么需要将最老的
  // LRUHandle节点移除, 直到 Cache 的总大小不溢出容量
  while (usage_ > capacity_ && lru_.next != &lru_) {
    LRUHandle* old = lru_.next;
    assert(old->refs == 1);
    bool erased = FinishErase(table_.Remove(old->key(), old->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }

  return reinterpret_cast<Cache::Handle*>(e);
}

// If e != nullptr, finish removing *e from the cache; it has already been
// removed from the hash table.  Return whether e != nullptr.
bool LRUCache::FinishErase(LRUHandle* e) {
  if (e != nullptr) {
    assert(e->in_cache);
    LRU_Remove(e);
    e->in_cache = false;
    usage_ -= e->charge;
    // 将引用计数减一, 如果减一后为零则销毁该缓存项
    Unref(e);
  }
  return e != nullptr;
}

void LRUCache::Erase(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  FinishErase(table_.Remove(key, hash));
}

// 源码注释
// 遍历lru_链表, 将该链表上的所有缓存项从Cache中移除
void LRUCache::Prune() {
  MutexLock l(&mutex_);
  while (lru_.next != &lru_) {
    LRUHandle* e = lru_.next;
    assert(e->refs == 1);
    bool erased = FinishErase(table_.Remove(e->key(), e->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }
}

static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits;

// 源码注释
// ShardedLRUCache封装了16个LRUCache缓存片
// 每次对缓存的读取、插入、查找、删除操作都是调用某个缓存片LRUCache中的相应方法完成
class ShardedLRUCache : public Cache {
 private:
  LRUCache shard_[kNumShards];
  port::Mutex id_mutex_;
  uint64_t last_id_;

  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }

  static uint32_t Shard(uint32_t hash) { return hash >> (32 - kNumShardBits); }

 public:
  explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].SetCapacity(per_shard);
    }
  }
  ~ShardedLRUCache() override {}
  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice& key, void* value)) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }
  Handle* Lookup(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Lookup(key, hash);
  }
  void Release(Handle* handle) override {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    shard_[Shard(h->hash)].Release(handle);
  }
  void Erase(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    shard_[Shard(hash)].Erase(key, hash);
  }
  void* Value(Handle* handle) override {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }
  uint64_t NewId() override {
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }
  void Prune() override {
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].Prune();
    }
  }
  size_t TotalCharge() const override {
    size_t total = 0;
    for (int s = 0; s < kNumShards; s++) {
      total += shard_[s].TotalCharge();
    }
    return total;
  }
};

}  // end anonymous namespace

Cache* NewLRUCache(size_t capacity) { return new ShardedLRUCache(capacity); }

}  // namespace leveldb
