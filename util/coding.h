// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Endian-neutral encoding:
// * Fixed-length numbers are encoded with least-significant byte first
// * In addition we support variable length "varint" encoding
// * Strings are encoded prefixed by their length in varint format

#ifndef STORAGE_LEVELDB_UTIL_CODING_H_
#define STORAGE_LEVELDB_UTIL_CODING_H_

#include <cstdint>
#include <cstring>
#include <string>

#include "leveldb/slice.h"
#include "port/port.h"

namespace leveldb {

// 源码注释
// LevelDB中为整型提供了两类编码方式, 一类是定长编码, 一类是变长编码
// LevelDB为了便于从字节数组中划分Slice, 其还提供了一种LengthPrefixedSlice的编码方式
// 在编码中将长度确定的Slice的长度作为Slice的前缀

// leveldb对于数字的定长编码存储是little-endian的
// 把一个int32或者int64格式化到字符串中, 除了上面说的little-endian字节序(定长编码)外
// 大部分还是变长存储的 也就是VarInt。
// 对于VarInt每byte的有效存储是7bit的, 用最高的8bit位来表示是否结束
// 如果是1就表示后面还有一个byte的数字, 否则表示结束

// FixedInt的编解码速度快, 但是会浪费空间, 属于空间换时间的做法。
// Varint的编解码速度慢, 但是节省空间, 属于时间换空间的做法。

// Standard Put... routines append to a string
void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);
// 带长度的Slice的编码方式非常简单, 只需要在原Slice之前加上用变长整型表示的Slice长度
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);

// Standard Get... routines parse a value from the beginning of a Slice
// and advance the slice past the parsed value.
bool GetVarint32(Slice* input, uint32_t* value);
bool GetVarint64(Slice* input, uint64_t* value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

// Pointer-based variants of GetVarint...  These either store a value
// in *v and return a pointer just past the parsed value, or return
// nullptr on error.  These routines only look at bytes in the range
// [p..limit-1]
const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* v);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* v);

// Returns the length of the varint32 or varint64 encoding of "v"
int VarintLength(uint64_t v);

// Lower-level versions of Put... that write directly into a character buffer
// and return a pointer just past the last byte written.
// REQUIRES: dst has enough space for the value being written
// 源码注释
// 整型变长编码
// 当整型值较小时, LevelDB支持将其编码为变长整型
// 以减少其空间占用（对于值与类型最大值接近时, 变长整型占用空间反而增加）
// 对于变长整型编码, LevelDB需要知道该整型编码的终点在哪儿。
// 因此LevelDB将每个字节的最高位作为标识符, 当字节最高位为1时表示编码未结束,
// 当字节最高位为0时表示编码结束。因此LevelDB的整型变长编码每8位用来表示整型值的7位。
// 因此，当整型值接近其类型最大值时，变长编码需要额外一字节来容纳原整型值。
// 同样，变长整型编码也采用了小端顺序
char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

// Lower-level versions of Put... that write directly into a character buffer
// REQUIRES: dst has enough space for the value being written

// 整型定长编码
// FixedInt就是将int32_t编码为一个4字节的序列, 将int64_t编码为一个8字节的序列
// 这种编码方式的优点是编码后的序列长度固定, 方便读取
// 缺点是对于较小的数值, 编码后的序列长度比实际需要的要大, 造成空间浪费
// LevelDB中整型的定长编码（32bits或64bits）方式非常简单，只需要将整型按照小端的顺序编码即可
inline void EncodeFixed32(char* dst, uint32_t value) {
  // 将char*转成uint8_t*, 避免溢出
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);

  // Recent clang and gcc optimize this to a single mov / str instruction.
  // 按照Little-Endian的方式将value写入buffer中
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
}

// 整型定长编码
// LevelDB中整型的定长编码（32bits或64bits）方式非常简单，只需要将整型按照小端的顺序编码即可
inline void EncodeFixed64(char* dst, uint64_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);

  // Recent clang and gcc optimize this to a single mov / str instruction.
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
  buffer[4] = static_cast<uint8_t>(value >> 32);
  buffer[5] = static_cast<uint8_t>(value >> 40);
  buffer[6] = static_cast<uint8_t>(value >> 48);
  buffer[7] = static_cast<uint8_t>(value >> 56);
}

// Lower-level versions of Get... that read directly from a character buffer
// without any bounds checking.

inline uint32_t DecodeFixed32(const char* ptr) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);

  // Recent clang and gcc optimize this to a single mov / ldr instruction.
  return (static_cast<uint32_t>(buffer[0])) |
         (static_cast<uint32_t>(buffer[1]) << 8) |
         (static_cast<uint32_t>(buffer[2]) << 16) |
         (static_cast<uint32_t>(buffer[3]) << 24);
}

inline uint64_t DecodeFixed64(const char* ptr) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);

  // Recent clang and gcc optimize this to a single mov / ldr instruction.
  return (static_cast<uint64_t>(buffer[0])) |
         (static_cast<uint64_t>(buffer[1]) << 8) |
         (static_cast<uint64_t>(buffer[2]) << 16) |
         (static_cast<uint64_t>(buffer[3]) << 24) |
         (static_cast<uint64_t>(buffer[4]) << 32) |
         (static_cast<uint64_t>(buffer[5]) << 40) |
         (static_cast<uint64_t>(buffer[6]) << 48) |
         (static_cast<uint64_t>(buffer[7]) << 56);
}

// Internal routine for use by fallback path of GetVarint32Ptr
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value);
inline const char* GetVarint32Ptr(const char* p, const char* limit,
                                  uint32_t* value) {
  if (p < limit) {
    uint32_t result = *(reinterpret_cast<const uint8_t*>(p)); // 解析第一个字节
    // 检查解析出来的字节最高位是否为0, 如果是则表示解析结束
    if ((result & 128) == 0) {
      *value = result;
      return p + 1;
    }
  }
  // 如果解析出来的字节的最高位不是0, 则代表后面还有字节需要解析
  return GetVarint32PtrFallback(p, limit, value);
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_CODING_H_
