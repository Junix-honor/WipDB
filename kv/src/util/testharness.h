// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_TESTHARNESS_H_
#define STORAGE_LEVELDB_UTIL_TESTHARNESS_H_

#include <stdio.h>
#include <stdlib.h>

#include <sstream>

#include "kv/status.h"

namespace kv {
namespace test {

// Run some of the tests registered by the TEST() macro.  If the
// environment variable "LEVELDB_TESTS" is not set, runs all tests.
// Otherwise, runs only the tests whose name contains the value of
// "LEVELDB_TESTS" as a substring.  E.g., suppose the tests are:
//    TEST(Foo, Hello) { ... }
//    TEST(Foo, World) { ... }
// LEVELDB_TESTS=Hello will run the first test
// LEVELDB_TESTS=o     will run both tests
// LEVELDB_TESTS=Junk  will run no tests
//
// Returns 0 if all tests pass.
// Dies or returns a non-zero value if some test fails.
int RunAllTests();

// Return the directory to use for temporary storage.
std::string TmpDir();

// Return a randomization seed for this run.  Typically returns the
// same number on repeated invocations of this binary, but automated
// runs may be able to vary the seed.
int RandomSeed();

// An instance of Tester is allocated to hold temporary state during
// the execution of an assertion.
class Tester {
 private:
  bool ok_;
  const char* fname_;
  int line_;
  std::stringstream ss_;

 public:
  Tester(const char* f, int l)
      : ok_(true), fname_(f), line_(l) {
  }

  ~Tester() {
    if (!ok_) {
      fprintf(stderr, "%s:%d:%s\n", fname_, line_, ss_.str().c_str());
      exit(1);
    }
  }

  Tester& Is(bool b, const char* msg) {
    if (!b) {
      ss_ << " Assertion failure " << msg;
      ok_ = false;
    }
    return *this;
  }

  Tester& IsOk(const Status& s) {
    if (!s.ok()) {
      ss_ << " " << s.ToString();
      ok_ = false;
    }
    return *this;
  }

#define BINARY_OP(name, op)                             \
  template <class X, class Y>                           \
  Tester& name(const X& x, const Y& y) {                \
    if (!(x op y)) {                                    \
      ss_ << " failed: " << x << (" " #op " ") << y;    \
      ok_ = false;                                      \
    }                                                   \
    return *this;                                       \
  }

  BINARY_OP(IsEq, ==)
  BINARY_OP(IsNe, !=)
  BINARY_OP(IsGe, >=)
  BINARY_OP(IsGt, >)
  BINARY_OP(IsLe, <=)
  BINARY_OP(IsLt, <)
#undef BINARY_OP

  // Attach the specified value to the error message if an error has occurred
  template <class V>
  Tester& operator<<(const V& value) {
    if (!ok_) {
      ss_ << " " << value;
    }
    return *this;
  }
};

#ifndef ASSERT_TRUE
#define ASSERT_TRUE(c) ::kv::test::Tester(__FILE__, __LINE__).Is((c), #c)
#endif

#define ASSERT_OK(s) ::kv::test::Tester(__FILE__, __LINE__).IsOk((s))

#ifndef ASSERT_EQ
#define ASSERT_EQ(a,b) ::kv::test::Tester(__FILE__, __LINE__).IsEq((a),(b))
#endif

#ifndef ASSERT_NE
#define ASSERT_NE(a,b) ::kv::test::Tester(__FILE__, __LINE__).IsNe((a),(b))
#endif

#ifndef ASSERT_GE
#define ASSERT_GE(a,b) ::kv::test::Tester(__FILE__, __LINE__).IsGe((a),(b))
#endif

#ifndef ASSERT_GT
#define ASSERT_GT(a,b) ::kv::test::Tester(__FILE__, __LINE__).IsGt((a),(b))
#endif

#ifndef ASSERT_LE
#define ASSERT_LE(a,b) ::kv::test::Tester(__FILE__, __LINE__).IsLe((a),(b))
#endif

#ifndef ASSERT_LT
#define ASSERT_LT(a,b) ::kv::test::Tester(__FILE__, __LINE__).IsLt((a),(b))
#endif

#define TCONCAT(a, b) TCONCAT1(a, b)
#define TCONCAT1(a, b) a##b


#define TEST_C(base, name)                                                       \
class TCONCAT(_Test_, name) : public base {                                    \
 public:                                                                       \
  void _Run();                                                                 \
  static void _RunIt() {                                                       \
    TCONCAT(_Test_, name) t;                                                   \
    t._Run();                                                                  \
  }                                                                            \
};                                                                             \
bool TCONCAT(_Test_ignored_, name) =                                           \
  ::kv::test::RegisterTest(#base, #name, &TCONCAT(_Test_, name)::_RunIt); \
void TCONCAT(_Test_, name)::_Run()


// Register the specified test.  Typically not used directly, but
// invoked via the macro expansion of TEST.
bool RegisterTest(const char* base, const char* name, void (*func)());

}  // namespace test
}  // namespace kv

#endif  // STORAGE_LEVELDB_UTIL_TESTHARNESS_H_
