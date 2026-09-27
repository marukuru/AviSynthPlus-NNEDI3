// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

using DotProduct = void (*)(const float *, const float *, float *, int, int, const float *);

// Returns nullptr if this build/CPU has no suitable SIMD implementation.
// These kernels use the same unshuffled weights as the C++ reference path.
DotProduct selectIntegerDotProduct(int cpu_flags, bool wide);

// -1 means a forced mode is unavailable. Automatic mode prefers FMA3 to FMA4.
int resolveOptimization(int requested, int cpu_flags, bool assembly_available);
int optimizationRequirements(int opt);
const char *optimizationName(int opt);
