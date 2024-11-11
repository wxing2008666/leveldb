// Copyright (c) 2018 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_NO_DESTRUCTOR_H_
#define STORAGE_LEVELDB_UTIL_NO_DESTRUCTOR_H_

#include <type_traits>
#include <utility>

namespace leveldb {

// 源码注释
// 模板类, 它用于包装一个实例, 使得其析构函数不会被调用
// https://selfboot.cn/2024/07/22/leveldb_source_nodestructor/

// Wraps an instance whose destructor is never called.
//
// This is intended for use with function-level static variables.
template <typename InstanceType>
class NoDestructor {
 public:
  template <typename... ConstructorArgTypes>
  explicit NoDestructor(ConstructorArgTypes&&... constructor_args) {
    // 第一个static_assert确保为InstanceType分配的存储空间instance_storage_至少要和
    // InstanceType实例本身一样大, 这是为了确保有足够的空间来存放该类型的对象
    // 第二个static_assert确保instance_storage_的对齐方式满足InstanceType的对齐要求
    // 对象只所以有内存对齐要求和性能有关
    static_assert(sizeof(instance_storage_) >= sizeof(InstanceType),
                  "instance_storage_ is not large enough to hold the instance");
    static_assert(
        alignof(decltype(instance_storage_)) >= alignof(InstanceType),
        "instance_storage_ does not meet the instance's alignment requirement");
    // C++的原地构造语法(placement new)。&instance_storage_提供了一个地址
    // 告诉编译器在这个已经分配好的内存地址上构造InstanceType的对象
    // 这样做避免了额外的内存分配, 直接在预留的内存块中构造对象
    // 接下来使用完美转发std::forward<ConstructorArgTypes>(constructor_args)...
    // 确保所有的构造函数参数都以正确的类型(保持左值或右值属性)传递给InstanceType的构造函数
    new (&instance_storage_)
        InstanceType(std::forward<ConstructorArgTypes>(constructor_args)...);
  }

  ~NoDestructor() = default;

  NoDestructor(const NoDestructor&) = delete;
  NoDestructor& operator=(const NoDestructor&) = delete;

  InstanceType* get() {
    return reinterpret_cast<InstanceType*>(&instance_storage_);
  }

 private:
  typename std::aligned_storage<sizeof(InstanceType),
                                alignof(InstanceType)>::type instance_storage_;
  // 源码注释
  // 前面placement new原地构造的时候用的内存地址由成员变量instance_storage_提供
  // instance_storage_的类型由 std::aligned_storage模板定义。这是一个特别设计的类型,
  // 用于提供一个可以安全地存储任何类型的原始内存块, 同时确保所存储的对象类型(这里是 InstanceType)具有适当的大小和对齐要求
  // 这里std::aligned_storage创建的原始内存区域和NoDestructor对象所在的内存区域一致
  // 也就是说如果NoDestructor被定义为一个函数内的局部变量, 那么它和其内的 instance_storage_都会位于栈上
  // 如果 NoDestructor被定义为静态或全局变量, 它和instance_storage_将位于静态存储区, 静态存储区的对象具有整个程序执行期间的生命周期

  // singleton对象是一个静态局部变量, 第一次调用BytewiseComparator()时被初始化, 它的生命周期和程序的整个生命周期一样长
  // 程序退出的时候, singleton对象本身会被析构销毁掉, 但是NoDestructor没有在其析构函数中添加任何逻辑来析构instance_storage_中构造的对象
  // 因此instance_storage_中的BytewiseComparatorImpl对象永远不会被析构
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_NO_DESTRUCTOR_H_
