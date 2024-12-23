// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/table_cache.h"

#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "util/coding.h"

namespace leveldb {

// 源码注释
// file字段表示SSTable相应的RandomAccessFile结构, 即SSTable在文件中的表示
// Table字段表示SSTable的Table结构, 其为SSTable在内存中的数据与接口
struct TableAndFile {
  RandomAccessFile* file;
  Table* table;
};

static void DeleteEntry(const Slice& key, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
  delete tf->table;
  delete tf->file;
  delete tf;
}

static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}

// 源码注释
// cache_的初始化构造一个LRUCache
// TableCache其实是一个包装类, 核心是cache_(LRUCache)
// TableCache的所有接口都是对LRUCache的封装, 方便使用
TableCache::TableCache(const std::string& dbname, const Options& options,
                       int entries)
    : env_(options.env),
      dbname_(dbname),
      options_(options),
      cache_(NewLRUCache(entries)) {}

TableCache::~TableCache() { delete cache_; }

// 源码注释
// LevelDB中会使用file_number给SortedTable编号。为了提高读取性能简化使用, LevelDB提供了TableCache用以缓存SortedTable及对应的.ldb 文件
// 尝试在cache_中查找指定的SST, 如果找到了就直接返回handle
// 如果没找到就打开这个SST, 并将其添加到cache_中然后再返handle
Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
  Status s;
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));
  *handle = cache_->Lookup(key);
  if (*handle == nullptr) {
    // 源码注释
    // 根据file_number构造出SST的文件名
    // 早期版本的LevelDB使用的是.sst后缀, 后来改为了.ldb
    // 为了兼容这两种命名方式, 这里会尝试两种后缀
    // TableFileName会构建.ldb后缀的SST文件名
    // SSTTableFileName会构建.sst后缀的SST文件名
    std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = nullptr;
    Table* table = nullptr;
    s = env_->NewRandomAccessFile(fname, &file);
    if (!s.ok()) {
      std::string old_fname = SSTTableFileName(dbname_, file_number);
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
        s = Status::OK();
      }
    }
    if (s.ok()) {
      s = Table::Open(options_, file, file_size, &table);
    }

    if (!s.ok()) {
      assert(table == nullptr);
      delete file;
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
    } else {
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
    }
  }
  return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number, uint64_t file_size,
                                  Table** tableptr) {
  if (tableptr != nullptr) {
    *tableptr = nullptr;
  }

  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }

  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  Iterator* result = table->NewIterator(options); // 调用Table::NewIterator方法创建该SST的Iterator
  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  if (tableptr != nullptr) {
    *tableptr = table;
  }
  return result;
}

// 源码注释
// 用于从Cache中查找指定的SST, 再从这个SST中查找指定的Key
Status TableCache::Get(const ReadOptions& options, uint64_t file_number,
                       uint64_t file_size, const Slice& k, void* arg,
                       void (*handle_result)(void*, const Slice&,
                                             const Slice&)) {
  // 源码注释
  // 在Cache中找到指定的SST, 如果目标SST不在Cache中, 它会打开文件并将其添加到Cache
  // handle指向Cache中的SST Item
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (s.ok()) {
    // 通过handle在cache中获取SST对应的Table对象
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    // 调用Table::InternalGet方法从SST中查找指定的key, 并将结果传递给handle_result
    s = t->InternalGet(options, k, arg, handle_result);
    cache_->Release(handle);
  }
  return s;
}

void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace leveldb
