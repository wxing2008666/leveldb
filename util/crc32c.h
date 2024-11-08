// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_CRC32C_H_
#define STORAGE_LEVELDB_UTIL_CRC32C_H_

#include <cstddef>
#include <cstdint>

// 源码注释
// 循环冗余校验
// CRC(Cyclic Redundancy Check, 循环冗余检查)是一种通过特定算法来计算数据的校验码的方法
// 广泛用于网络通讯和数据存储系统中以检测数据在传输或存储过程中是否发生错误
// CRC32是一种常见的CRC算法, 使用了一个32位的校验和

// 代码中具体实现比较比较复杂, 涉及到查找表(table-driven approach)、数据对齐、和可能的硬件加速
// 具体的原理可以参考: http://www.ross.net/crc/download/crc_v3.txt
// 其中生成多项式的选择对CRC算法的有效性和错误检测能力至关重要; 生成多项式并不是随意选取的, 它们通常通过数学和计算机模拟实验被设计出来
// 以确保最大化特定数据长度和特定应用场景下的错误检测能力, 常见的生成多项式0x04C11DB7就是在IEEE 802.3标准中为CRC-32算法选定的
// CRC只是用来检测随机错误, 比如网络传输或者磁盘存储中某些比特位发生了翻转。它不是纠错校验码, 只能检测到错误, 并不能纠正错误。
// 我们可以故意对内容进行篡改然后保证CRC结果一样, 如果要防篡改, 要用到更为复杂的加密哈希函数或者数字签名技术

namespace leveldb {
namespace crc32c {

// Return the crc32c of concat(A, data[0,n-1]) where init_crc is the
// crc32c of some string A.  Extend() is often used to maintain the
// crc32c of a stream of data.
// 源码注释
// CRC的计算基于多项式除法, 处理的数据被视为一个巨大的多项式, 通过这个多项式除以另一个预定义的"生成多项式"
// 然后取余数作为输出的CRC值; CRC算法具有天然的流式计算特性, 可以先计算消息的一部分的CRC, 然后将这个结果作为下一部分计算的初始值(init_crc)
// Extend函数接受一个初始的CRC值(可能是之前数据块的CRC结果)然后计算加上新的数据块data后的CRC值
// 这使得LevelDB能够在不断追加数据时连续计算CRC, 而不需要每次都从头开始
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

// Return the crc32c of data[0,n-1]
inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

static const uint32_t kMaskDelta = 0xa282ead8ul;

// Return a masked representation of crc.
//
// Motivation: it is problematic to compute the CRC of a string that
// contains embedded CRCs.  Therefore we recommend that CRCs stored
// somewhere (e.g., in files) should be masked before being stored.
// 源码注释
// 如果数据本身包含CRC值, 然后直接在包含CRC的数据上再次计算CRC, 可能会降低CRC的错误检测能力
// 因此LevelDB对CRC值进行高低位交换后加上一个常数(kMaskDelta)来"掩码"原始的CRC值
// 这种变换后的CRC值可以存储在文件中, 当要验证数据完整性时使用Unmask函数将掩码后的CRC值转换回原始的CRC值
// 再与当前数据的CRC计算结果进行比较
// 原始CRC32值交换高15位后, 加上常量后可能会大于uint32_t的最大值导致溢出; 在C++中无符号整型的溢出行为是定义良好的
// 按照取模运算处理, 比如当前crc是32767, 这里移动后加上常量结果是7021325016
// 按照2^32取模后结果是2726357720。而在Unmask中的减法操作同样会溢出, C++中这里也是按照取模运算处理的。
// 这里2726357720 − kMaskDelta = −131072 按照2^32后结果是4294836224
// 再交换高低位就拿到了原始CRC 32767 了, 所以这里的溢出不会导致 bug
inline uint32_t Mask(uint32_t crc) {
  // Rotate right by 15 bits and add a constant.
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

// Return the crc whose masked representation is masked_crc.
inline uint32_t Unmask(uint32_t masked_crc) {
  uint32_t rot = masked_crc - kMaskDelta;
  return ((rot >> 17) | (rot << 15));
}

}  // namespace crc32c
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_CRC32C_H_
