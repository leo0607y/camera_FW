#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
inline bool mock_allocation_ok=true;
inline void *heap_caps_malloc(size_t size,int) { return mock_allocation_ok ? std::malloc(size) : nullptr; }
