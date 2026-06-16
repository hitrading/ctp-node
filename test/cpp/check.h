/*
 * check.h - a tiny assertion harness for the standalone C++ unit tests, matching
 * the hand-rolled style of the JS tests (no gtest/Catch2 dependency). Each test
 * file has a main() that ends with REPORT(...).
 */
#ifndef CTP_TEST_CHECK_H
#define CTP_TEST_CHECK_H

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                        \
  do {                                                                          \
    if (cond) {                                                                 \
      ++g_pass;                                                                 \
      std::printf("  PASS: %s\n", msg);                                         \
    } else {                                                                    \
      ++g_fail;                                                                 \
      std::printf("  FAIL: %s\n", msg);                                         \
    }                                                                           \
  } while (0)

#define REPORT(name)                                                            \
  do {                                                                          \
    std::printf("%s: %d pass, %d fail\n", name, g_pass, g_fail);                \
    return g_fail ? 1 : 0;                                                      \
  } while (0)

#endif /* CTP_TEST_CHECK_H */
