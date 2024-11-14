// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/filename.h"

#include <cassert>
#include <cstdio>

#include "db/dbformat.h"
#include "leveldb/env.h"
#include "util/logging.h"

namespace leveldb {

// 源码注释
// 000001.log是WAL文件, 写入的键值对, 都会先写到这个日志文件中
// 000005.ldb是SSTable文件, 存储了持久化到磁盘的键值对
// LOCK文件是锁文件, 一个LevelDB数据库同时只允许被一个进程操作, 一个进程打开一个数据库时会对这个文件加锁, 防止其它进程并发打开这个数据库
// LOG是通用日志文件, 在里面打印一些系统运行的信息
// MANIFEST-000002是资源文件, 记录了版本信息, LevelDB有一系列的ldb文件, 各个文件在不同的Level, 而资源文件记录了当前各个文件在哪一层, 下一个待分配的文件编号是什么等信息
// CURRENT, 在进行一次Compaction后生成新的版本信息, 会将变化写入到MANIFEST文件中, 如果MANIFEST太大下次打开时会重写MANIFEST文件, 会新增一个MANIFEST
// 而CURRENT则保存了当前使用的MANIFEST文件, 是为了安全地重写MANIFEST文件

// A utility routine: write "data" to the named file and Sync() it.
Status WriteStringToFileSync(Env* env, const Slice& data,
                             const std::string& fname);

static std::string MakeFileName(const std::string& dbname, uint64_t number,
                                const char* suffix) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s",
                static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

std::string LogFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "ldb");
}

std::string SSTTableFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "sst");
}

std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu",
                static_cast<unsigned long long>(number));
  return dbname + buf;
}

std::string CurrentFileName(const std::string& dbname) {
  return dbname + "/CURRENT";
}

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

std::string TempFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "dbtmp");
}

std::string InfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG";
}

// Return the name of the old info log file for "dbname".
std::string OldInfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG.old";
}

// Owned filenames have the form:
//    dbname/CURRENT
//    dbname/LOCK
//    dbname/LOG
//    dbname/LOG.old
//    dbname/MANIFEST-[0-9]+
//    dbname/[0-9]+.(log|sst|ldb)
bool ParseFileName(const std::string& filename, uint64_t* number,
                   FileType* type) {
  Slice rest(filename);
  if (rest == "CURRENT") {
    *number = 0;
    *type = kCurrentFile;
  } else if (rest == "LOCK") {
    *number = 0;
    *type = kDBLockFile;
  } else if (rest == "LOG" || rest == "LOG.old") {
    *number = 0;
    *type = kInfoLogFile;
  } else if (rest.starts_with("MANIFEST-")) {
    rest.remove_prefix(strlen("MANIFEST-"));
    uint64_t num;
    if (!ConsumeDecimalNumber(&rest, &num)) {
      return false;
    }
    if (!rest.empty()) {
      return false;
    }
    *type = kDescriptorFile;
    *number = num;
  } else {
    // Avoid strtoull() to keep filename format independent of the
    // current locale
    uint64_t num;
    if (!ConsumeDecimalNumber(&rest, &num)) {
      return false;
    }
    Slice suffix = rest;
    if (suffix == Slice(".log")) {
      *type = kLogFile;
    } else if (suffix == Slice(".sst") || suffix == Slice(".ldb")) {
      *type = kTableFile;
    } else if (suffix == Slice(".dbtmp")) {
      *type = kTempFile;
    } else {
      return false;
    }
    *number = num;
  }
  return true;
}

Status SetCurrentFile(Env* env, const std::string& dbname,
                      uint64_t descriptor_number) {
  // Remove leading "dbname/" and add newline to manifest file name
  std::string manifest = DescriptorFileName(dbname, descriptor_number);
  Slice contents = manifest;
  assert(contents.starts_with(dbname + "/"));
  contents.remove_prefix(dbname.size() + 1);
  std::string tmp = TempFileName(dbname, descriptor_number);
  Status s = WriteStringToFileSync(env, contents.ToString() + "\n", tmp);
  if (s.ok()) {
    s = env->RenameFile(tmp, CurrentFileName(dbname));
  }
  if (!s.ok()) {
    env->RemoveFile(tmp);
  }
  return s;
}

}  // namespace leveldb
