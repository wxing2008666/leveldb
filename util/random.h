// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_RANDOM_H_
#define STORAGE_LEVELDB_UTIL_RANDOM_H_

#include <cstdint>

namespace leveldb {

// A very simple random number generator.  Not especially good at
// generating truly random bits, but good enough for our needs in this
// package.
// 源码注释
// 简单的伪随机数生成器。对于生成真正随机位来说，不是特别优秀，但足以满足本包中需要的功能
// 基于线性同余法, seed = (seed ∗ A + C) % M, 根据当前伪随机数生成下一个伪随机数
// 其中A,C,M都是常数（一般会取质数）, 当C=0时, 叫做乘同余法
// leveldb中, 乘法因子A = 16807, 模数M = 2147483647, 增量C = 0
// 之所以取这组系数是因为可以生成均匀分布的伪随机数
// 计算机几乎不可能实现真正的随机性，因为这些随机数字是由确定的算法产生的，计算机生成的是伪随机数。
// 只不过这些看起来像随机的数，满足随机数的有许多已知的统计特性，在程序中被当作随机数使用
class Random {
 private:
 // 源码注释
 // 随机数生成器的种子, 无符号32位整数, 范围[0, 2^32-1]即[0, 4294967295]
 // N位能表示的最大数为 2^N-1
  uint32_t seed_;

 public:
  // 源码注释
  // 构造函数, s为32位无符号整数, 0x7fffffffu是32位二进制数, 最高位为0其余各位全为1
  // 无符号转化为有符号的时候头部为1, 表示为负数
  // 按位与的目的是避免种子为负数或0
  // 如果seed_weed为0或2147483647, 则每次生成的随机数为固定值
  // seed_ must not be zero or M
  explicit Random(uint32_t s) : seed_(s & 0x7fffffffu) {
    // Avoid bad seeds.
    if (seed_ == 0 || seed_ == 2147483647L) {
      seed_ = 1;
    }
  }
  uint32_t Next() {
    static const uint32_t M = 2147483647L;  // 2^31-1
    static const uint64_t A = 16807;        // bits 14, 8, 7, 5, 2, 1, 0
    // We are computing
    //       seed_ = (seed_ * A) % M,    where M = 2^31-1
    //
    // seed_ must not be zero or M, or else all subsequent computed values
    // will be zero or M respectively.  For all other values, seed_ will end
    // up cycling through every number in [1,M-1]
    uint64_t product = seed_ * A;

    // Compute (product % M) using the fact that ((x << 31) % M) == x.
    // 源码注释
    // 这里是计算Mod算法的一个优化, 一个64位的数, 可以分为高33位和低31位的数
    // product=high<<31+low 又因为product=seed_*A, 所以此时product=(high*M+high+low)%M 其中 M = 2^31-1
    // 因为seed_和A中, seed_的值在初始化的时候就让他小于2^31-1, A是固定的16807,所以这两个值都不会大于M的值
    // 所以取余最后的结果就等(high+low)%M=high+low,所以下面的这个计算是获取high和low的值相加,也就得到了seed_
    // 但是有一种情况就是product的low为刚好但是2^31-1, 这个时候product=(high*M+high+1*M)%M=high
    // 但是使用下面的结果会是high+M,因为M&M=M。所以需要判断是否seed_比M大, 然后前去M, 直接使用high,也确保了seed的值一直小于M
    // 最后强制转换为32位的值
    // 技巧，即先计算乘积，然后通过位移和位与操作来减少模运算的复杂度。如果最终的结果大于M，则通过减去M来调整
    seed_ = static_cast<uint32_t>((product >> 31) + (product & M));
    // The first reduction may overflow by 1 bit, so we may need to
    // repeat.  mod == M is not possible; using > allows the faster
    // sign-bit-based test.
    if (seed_ > M) {
      seed_ -= M;
    }
    return seed_;
  }
  // Returns a uniformly distributed value in the range [0..n-1]
  // REQUIRES: n > 0
  // 源码注释
  // 生成一个在[0, n-1]范围内的均匀分布的随机数
  uint32_t Uniform(int n) { return Next() % n; }

  // Randomly returns true ~"1/n" of the time, and false otherwise.
  // REQUIRES: n > 0
  // 源码注释
  // 以1/n的概率返回true, 否则返回false
  bool OneIn(int n) { return (Next() % n) == 0; }

  // Skewed: pick "base" uniformly from range [0,max_log] and then
  // return "base" random bits.  The effect is to pick a number in the
  // range [0,2^max_log-1] with exponential bias towards smaller numbers.
  // 源码注释
  // 函数生成一个带有指数偏差的随机数。
  // 它首先使用Uniform(max_log + 1)生成一个[0, max_log]范围内的随机数base, 表示要生成的随机数的位数（以2为底的对数）
  // 然后，它再次使用Uniform函数, 但这次是在[0, 2^base - 1]范围内生成随机数。
  // 这种方式倾向于生成较小的随机数, 因为较小的base值出现的概率更高。
  uint32_t Skewed(int max_log) { return Uniform(1 << Uniform(max_log + 1)); }
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_RANDOM_H_
