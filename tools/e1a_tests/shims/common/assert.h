// Test-only assertion plumbing. Packet branches and bit fields use real source.
#pragma once
#include <cstdlib>
#define ASSERT(expr)                                                           \
  do {                                                                         \
    if (!(expr))                                                               \
      std::abort();                                                            \
  } while (false)
#define ASSERT_MSG(expr, ...) ASSERT(expr)
#define UNREACHABLE() std::abort()
#define UNREACHABLE_MSG(...) std::abort()
