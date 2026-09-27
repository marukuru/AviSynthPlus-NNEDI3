> [!NOTE]
> **AI-assisted development: CODEX GPT-6 Astra Extra High**

# NNEDI3 for AviSynth+

NNEDI3 deinterlacing and power-of-two enlargement, based on the 0.9.4.69
Windows plugin. This tree also builds a native Linux AviSynth+ plugin, including
8–16-bit integer and 32-bit float processing, planar YUV/RGB with alpha, packed
RGB, YUY2, and `nnedi3_rpow2`.

Build on Linux with CMake 3.16+, GCC or Clang, NASM, Python 3, and AviSynth+
headers supporting interface version 8 or later. The neural-network weights are embedded in the
library; no separate weights file is needed at runtime.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DAVISYNTH_SOURCE_DIR=/home/user/workspace/compile/avisynthplus/AviSynthPlus
cmake --build build -j
```

The output is `build/libnnedi3.0.9.4.69.so`. The filename always follows
`libnnedi3.<project version>.so`. If AviSynth+ development headers are installed,
`AVISYNTH_SOURCE_DIR` can be omitted. Alternatively, set `AVISYNTH_INCLUDE_DIR`
to the directory containing `avisynth.h` and `avs/`.

Load the library explicitly, or install it in an AviSynth+ plugin directory:

```avs
LoadPlugin("/absolute/path/to/libnnedi3.0.9.4.69.so")
# source = ...
source.nnedi3(field=1, threads=1).Prefetch(4)
```

```sh
cmake --install build --prefix "$HOME/.local"
```

The default install destination is `<prefix>/<libdir>/avisynth`. Set
`AVISYNTH_PLUGIN_DIR` at configure time to choose a different directory. Explicit
`LoadPlugin` works regardless of whether that directory is in the host's autoload
search path. Keep only one NNEDI3 version in an autoload directory.

Linux threading and optimization:

- `threads=1` is the Linux default. Use AviSynth+ `Prefetch(N)` to process frames
  concurrently. Both factories advertise `MT_MULTI_INSTANCE`, including chains
  ending in a color converter or resizer, so each instance owns its scratch data.
- `threads=N` enables internal parallel processing. Persistent workers sleep
  between passes, and the calling thread also processes a slice. The prediction
  pass balances slices using the prescreener's count of pixels needing prediction.
  Small frames limit the worker count to avoid empty or excessively small slices.
- `threads=0` selects CPUs available to the process (respecting Linux affinity).
  `logicalCores=false` selects physical cores. The plugin's `prefetch` argument
  divides that automatic CPU budget; for example, `threads=0, prefetch=4` reserves
  roughly a quarter of the CPUs per instance. It does **not** invoke AviSynth+
  `Prefetch`. With an explicit thread count, the approximate processing budget is
  `threads * Prefetch`, so tune both together.
- `opt=0` selects the best available SIMD mode using AviSynth+'s CPU/OS flags.
  Forced modes are checked before processing; unsupported instructions produce
  an AviSynth error. `opt=1` is the C++ reference implementation.
- `sleep`, `SetAffinity`, `MaxPhysCore`, and `ThreadLevel` remain accepted for
  script compatibility. Linux workers always sleep when idle and use normal OS
  scheduling; these Windows pool controls have no effect. The sign of the
  plugin's `prefetch` argument has no effect on Linux.

On x86-64, all original optimization modes are available:

| `opt` | Kernels | Required CPU/OS features |
|---|---|---|
| 0 | Automatic | Chooses 8, 6, 7, 5, 4, 3, 2, then 1 |
| 1 | C++ reference | None beyond the build target |
| 2 | SSE2 | SSE2 |
| 3 | SSE4.1 | SSE2, SSE4.1 |
| 4 | AVX | SSE4.1, AVX |
| 5 | AVX2 | SSE4.1, AVX, AVX2 |
| 6 | FMA3 | AVX2 requirements plus FMA3 |
| 7 | FMA4 | AVX2 requirements plus FMA4 |
| 8 | AVX512 | FMA3/AVX2 requirements plus AVX512F, DQ, BW, VL |

A CPU with the listed SSE4.2, AVX, AVX2 and FMA3 capabilities uses `opt=6`
automatically. MMX2, SSE2Fast, SSSE3 and BMI2 need no separate setting.
The higher modes share lower-ISA helpers, hence the prerequisite combinations.
FMA4 is its own implementation; it is not emulated with FMA3.

The Linux build translates the original x64 MASM kernels into NASM ELF objects
and generates compiler-managed System V / Win64 calling-convention adapters.
This preserves the optimized prescreeners, pixel conversion, neighborhood
extraction, prediction, exponential and weighted-average kernels. It needs no
Windows runtime. ISA-specific instructions stay out of the baseline dispatch;
`-march=native` is unnecessary.

As in the original plugin, `nsize=0` uses AVX2/FMA3 helpers in AVX512 mode because
its 48-sample window is not a multiple of 32. On Linux, integer prescreening of
15/16-bit pixels uses unsigned, wide C++ accumulation to preserve the full
input range. Other stages still use the selected SIMD mode. SIMD approximation
and reduction order can produce small numerical differences from `opt=1`;
changing the number of threads preserves output exactly within a mode.

Set `-DNNEDI3_X86_ASM=OFF` to build without NASM/Python, or on other little-endian
architectures. That build supports `opt=0/1`, retaining the portable SSE2/AVX2
integer dot products where available. Forced modes 2–8 require the x64 assembly
build. All weights remain embedded in either build.

For internal threading without frame prefetch, use
`source.nnedi3(field=1, threads=4)`. For enlargement, use
`source.nnedi3_rpow2(rfactor=2, threads=1).Prefetch(4)`.
Other filter parameters are described in [the original readme](nnedi3%20-%20Readme.txt).
Ignored planes retain the original plugin's unspecified-content behavior.

Optional tests require the AviSynth+ runtime library:

```sh
cmake -S . -B build -DNNEDI3_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
build/nnedi3_integration "$PWD/build/libnnedi3.0.9.4.69.so" --benchmark
```

Tests use deterministic patterned frames and compare C++ and every SIMD mode
supported by the host, with numerical tolerances for approximation differences.
Internal threading and `Prefetch` comparisons require exact output. They cover
pixel formats, field retention, frame properties, prescreeners, all network
window sizes, approximation modes, `nnedi3_rpow2`, narrow planes, tiny frames,
worker exception handling, and rejection of unavailable ISAs. Synthetic CPU
masks also check FMA4/AVX512 selection and all required feature combinations.
The benchmark reports each available mode on 640×360 Y8, Y16 and Y32 clips,
then automatic SIMD with 1, 2, 4 and 8 internal threads. Measure on your own
material when choosing thread counts.

GCC and Clang builds have been tested on x86-64 with SSE2 through AVX2/FMA3.
FMA4 and AVX512 kernels assemble and link, but require suitable hardware for
execution testing. Other little-endian architectures have not been validated
here. The original Windows Visual Studio/MASM build remains in `nnedi3.sln`;
it is not built by CMake and was not tested as part of this port.

Licensed under GPL-2.0-or-later; see [LICENSE](LICENSE).
