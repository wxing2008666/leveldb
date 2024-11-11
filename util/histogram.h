// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_HISTOGRAM_H_
#define STORAGE_LEVELDB_UTIL_HISTOGRAM_H_

#include <string>

namespace leveldb {

// 源码注释
// 直方图类, 用于统计数据分布情况, 提供给性能评测工具使用
class Histogram {
 public:
  Histogram() {}
  ~Histogram() {}

  void Clear();
  void Add(double value);
  void Merge(const Histogram& other);

  std::string ToString() const;

 private:
  enum { kNumBuckets = 154 };

  // 中位数
  double Median() const;
  // 百分位数
  double Percentile(double p) const;
  // 平均值
  double Average() const;
  // 标准偏差
  double StandardDeviation() const;

  static const double kBucketLimit[kNumBuckets];

  double min_;
  double max_;
  double num_;
  double sum_;
  double sum_squares_;

  double buckets_[kNumBuckets];
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_HISTOGRAM_H_
