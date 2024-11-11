// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_HELPERS_MEMENV_MEMENV_H_
#define STORAGE_LEVELDB_HELPERS_MEMENV_MEMENV_H_

#include "leveldb/export.h"

namespace leveldb {

// A pure in-memory environment
// 源码注释
// 提供了一个完全基于内存的文件系统接口
// 这对于单元测试和性能分析是很有用的, 使用memenv就无需在单元测试中去做文件的创建和删除
// 同时性能分析中也可以去除磁盘的影响, 可以方便地了解到各种操作在CPU方面的性能
// 同时也可以与磁盘文件做性能对比分析以了解IO方面的开销。通过这个InMemoryEnv用户就可以将LevelDB架在内存

class Env;

// Returns a new environment that stores its data in memory and delegates
// all non-file-storage tasks to base_env. The caller must delete the result
// when it is no longer needed.
// *base_env must remain live while the result is in use.
LEVELDB_EXPORT Env* NewMemEnv(Env* base_env);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_HELPERS_MEMENV_MEMENV_H_
