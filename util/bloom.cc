// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/filter_policy.h"

#include "leveldb/slice.h"
#include "util/hash.h"

namespace leveldb {

// 源码注释
// 在LevelDB中查询某个指定key = query对应的value, 如果我们事先知道了所有的key里都找不到这个query
// 那也就不需要进一步的读取磁盘、精确查找了, 可以有效地减少磁盘访问数量
// FilterPolicy就负责这件事情:
// 它可以根据一组key创建一个小的过滤器filter, 并且可以将该过滤器和键值对存储在磁盘中
// 在查询时快速判断query是否在filter中。默认使用的FilterPolicy即为布隆过滤器

// 对于布隆过滤器, 如果Key在filter里, 那么一定会Match上;
// 反之如果不在, 那么有小概率也会Match上, 进而会多做一些磁盘访问
// 只要这个概率足够小也无伤大雅。这也刚好符合KeyMayMatch函数的语义

namespace {
static uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}

class BloomFilterPolicy : public FilterPolicy {
 public:
  explicit BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
    // We intentionally round down to reduce probing cost a little bit
    // 要使得Bloom Filter的误判率最小, k_的计算方法为:
    // k_ = (size_of_bloom_filter_in_bits/number_of_keys) * ln(2)
    // 即k_ = bits_per_key * ln(2)
    k_ = static_cast<size_t>(bits_per_key * 0.69);  // 0.69 =~ ln(2)
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
  }

  const char* Name() const override { return "leveldb.BuiltinBloomFilter2"; }

  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    // Compute bloom filter size (in both bits and bytes)
    // 源码注释: 计算布隆过滤器的大小
    // bits = key的数量 * 每个key占用的bit数
    size_t bits = n * bits_per_key_;

    // For small n, we can see a very high false positive rate.  Fix it
    // by enforcing a minimum bloom filter length.
    // 源码注释: 对于小n, 可能会看到非常高的误判率。
    // 通过强制最小布隆过滤器长度来修复它
    if (bits < 64) bits = 64;

    // 将给定的比特数（bits）转换成等价的字节数（bytes）
    // 为什么要加7呢？这是因为当我们把比特数转换成字节数时
    // 如果有剩余的比特（即比特数不是8的倍数），这些剩余的比特也需要占用一个字节的空间。
    // 通过加7，我们确保即使bits是一个很小的数（比如1到7），结果也会是至少一个字节（8比特）所需的数
    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    // dst里可能已经有其他fitler了, 先将dst的容量扩大bytes
    // 把当前 filter 的空间给创建出来
    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    // 在filter的末尾压入k_, 以供KeyMayMatch解码filter的时候使用
    dst->push_back(static_cast<char>(k_));  // Remember # of probes in filter
    // 获取当前BloomFilter在dst中的起始位置
    char* array = &(*dst)[init_size];
    for (int i = 0; i < n; i++) {
      // Use double-hashing to generate a sequence of hash values.
      // See analysis in [Kirsch,Mitzenmacher 2006].
      // 源码注释: 使用双哈希来生成一系列的hash值
      // 使用 double-hashing 来计算每个 key 的 hash 值
      // double-hashing 是一种优秀的 hash 实现算法
      // 能够在输入不均匀分布的情况下，提供均匀分布的 hash
      // double-hashing 的详情可见原文: https://www.eecs.harvard.edu/~michaelm/postscripts/rsa2008.pdf
      uint32_t h = BloomHash(keys[i]);
      const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
      for (size_t j = 0; j < k_; j++) {
        const uint32_t bitpos = h % bits;
        // 设置 Bloom Filter 中对应的 bit 位为 1
        array[bitpos / 8] |= (1 << (bitpos % 8));
        h += delta;
      }
    }
  }

  bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const override {
    const size_t len = bloom_filter.size();
    if (len < 2) return false;

    const char* array = bloom_filter.data();
    // len - 1 是因为 array 里的最后一个字节是 k_
    const size_t bits = (len - 1) * 8;

    // Use the encoded k so that we can read filters generated by
    // bloom filters created using different parameters.
    // 取出该 bloom_filter 的 k_ 值，以供解码 bloom_filter 使用
    const size_t k = array[len - 1];
    if (k > 30) {
      // Reserved for potentially new encodings for short bloom filters.
      // Consider it a match.
      return true;
    }

    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
    for (size_t j = 0; j < k; j++) {
      const uint32_t bitpos = h % bits;
      if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
      h += delta;
    }
    return true;
  }

 private:
  size_t bits_per_key_; // 每个key占用的bit数
  size_t k_; // Bloom Filter使用的hash函数个数
};
}  // namespace

const FilterPolicy* NewBloomFilterPolicy(int bits_per_key) {
  return new BloomFilterPolicy(bits_per_key);
}

}  // namespace leveldb
