// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifdef _WIN32
#include <windows.h>
#include "avisynth.h"
#else
#include <avisynth.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

using std::min;
using std::max;

inline void *_aligned_malloc(size_t size, size_t alignment)
{
    void *ptr = nullptr;
    return posix_memalign(&ptr, alignment, size) == 0 ? ptr : nullptr;
}

inline void _aligned_free(void *ptr) { std::free(ptr); }
#endif
