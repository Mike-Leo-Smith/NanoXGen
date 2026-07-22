# LuisaCompute `next` execution backend

NanoXGen can consume a separately built LuisaCompute `next` tree as an explicit
JIT execution backend. It is never downloaded, discovered, or linked by the
default CPU, CUDA, HIP, or Autodesk presets. LuisaCompute sources, submodules,
shader caches, runtime plug-ins, and build products must remain outside this
repository.

The tested upstream revision is
`c07bc0824aebb97784cdaff6e2c34b77acab20a8`. The `next` branch moves, so record
the exact revision used for every benchmark.

## AMD HIP build

Clone recursively outside NanoXGen, then use the checked-in helper to configure
only the WIP HIP backend:

```bash
git clone --branch next --recursive \
  https://github.com/LuisaGroup/LuisaCompute.git \
  /external/LuisaCompute-next

NANOXGEN_HIP_ARCH=gfx1201 \
./scripts/build_luisa_compute_next.sh \
  /external/LuisaCompute-next \
  /external/LuisaCompute-next/build-nanoxgen-hip
```

LuisaCompute `next` currently uses an LLVM API newer than the headers bundled
with some ROCm releases. The helper deliberately selects system LLVM through
`LLVM_DIR` while still finding HIP/HIPRTC and AMD device libraries under
`ROCM_PATH` (defaults: `/usr/lib/cmake/llvm` and `/opt/rocm`). Override either
environment variable for another installation. This is an isolated build
choice; no Maya or system libraries are copied or replaced.

Configure NanoXGen against the source and build trees:

```bash
export LUISA_COMPUTE_SOURCE_DIR=/external/LuisaCompute-next
export LUISA_COMPUTE_BUILD_DIR=/external/LuisaCompute-next/build-nanoxgen-hip

cmake --preset luisa-hip-release
cmake --build --preset luisa-hip-release
ctest --preset luisa-hip-release
```

The imported CMake adapter consumes LuisaCompute's narrow DSL/runtime shared
libraries and loads the selected backend plug-in at runtime. It does not use
`add_subdirectory`: the upstream HIP build currently refers to its top-level
source directory while compiling HIPRT, so a standalone upstream build is the
reproducible integration boundary.

## Current proof and next lowering stage

`nanoxgen_luisa_tests` records a C++ kernel into Luisa AST/XIR, JIT-compiles it,
uploads 65,536 inputs, evaluates deterministic integer hash and float width
operations, downloads both outputs, and compares every value with the CPU
oracle. It prints compilation time, upload/dispatch/download time, and a stable
checksum. A second process also exercises LuisaCompute's shader cache.

This proves that the actual LuisaCompute HIP runtime works on the selected AMD
device; it is not yet a claim that a complete Classic XGen description is
natively evaluable. The next layer lowers the bounded Classic expression and
generation IR into Luisa callables/kernels. Unsupported SeExpr, PTEX, module,
or topology operations remain checked errors and select the Autodesk fallback
until their CPU oracle and Luisa differential tests pass.
