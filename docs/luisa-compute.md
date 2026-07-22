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

## Float-only expression lowering

`nanoxgen_luisa_tests` records C++ kernels into Luisa AST/XIR, JIT-compiles them,
uploads 65,536 inputs, evaluates deterministic integer hash and float width
operations, downloads both outputs, and compares every value with the CPU
oracle. It also compiles a bounded Classic scalar expression, lowers that SSA
program into the same Luisa kernel, binds it through
`ClassicFloatRuntimeExpression` (`id`, `cLength`, and `cWidth`), and checks
every downloaded result. The direct binding form records values from the
surrounding generation kernel and does not require a device-side interpreter
or a temporary expression-input buffer. The test prints compilation time,
upload/dispatch/download time, expression dispatch time, error bounds, and a
stable checksum. A second process also exercises LuisaCompute's shader cache.

When HIP is enabled by `luisa-hip-release`, the same test allocates renderer
points and root records with HIP, runs the native packed generator, imports
those exact device pointers with LuisaCompute, and executes primitive length,
reparameterized Cut, and width/taper/ramp kernels without a host copy between
stages. It compares every final `float4(position, radius)` against the CPU plan.
The July 2026 RX 9070 XT fixture's maximum absolute error was `4.76837e-7`.

The runtime boundary is deliberately typed: `XgenFloatExpressionProgram`
contains float immediates and the Luisa lowering accepts only that program
type. Device contexts, inputs, expression values, hashes, and random values use
only `float` and `uint32`. The strict Autodesk/SeExpr calibration evaluator is
a separate CPU path that retains `double`, because replacing it would make the
oracle itself inaccurate. It is converted once to the compact float runtime IR
and is never captured by a Luisa callable or kernel.

The fast float hash/random sequence is NanoXGen-stable but is not bit-compatible
with Autodesk's double-based SeExpr sequence. This is an explicit execution
mode, not an unnoticed precision change. Strict Autodesk acceptance tests keep
using the CPU calibration mode.

For a generated-code audit, LuisaCompute's HIP backend can dump its optimized
LLVM IR outside the repository:

```bash
mkdir -p /tmp/nanoxgen-llvm-audit
cd /tmp/nanoxgen-llvm-audit
LUISA_DUMP_LLVM_IR=1 \
  /path/to/nanoxgen_luisa_tests /path/to/luisa/runtime hip
rg 'double|f64|llvm\.fma\.f64' hip_kernel_after_opt_*.ll
```

No match is expected for NanoXGen's kernels. The July 2026 RX 9070 XT
(`gfx1201`) run produced no FP64 match in either optimized kernel.

This proves that the actual LuisaCompute HIP runtime works on the selected AMD
device and that the supported bounded Classic runtime plan is JIT-lowered, not
interpreted on the host. Authored `rampUI` values are parsed into constant
control points and lowered with flat, linear, smooth, and spline interpolation.
This is not yet a claim that a complete Classic XGen description is natively
evaluable.
Unsupported SeExpr, PTEX, modules, and topology operations remain checked errors
and select the Autodesk fallback until their CPU oracle and Luisa differential
tests pass.

## Full Rabbit engineering benchmark

`nanoxgen_luisa_classic_benchmark` measures native HIP packed generation plus
the supported authored Classic float plan in one HIP stream. All descriptions
remain resident for each full-asset sample. File I/O, GPU allocation, asset
upload, JIT compilation, and the final checksum download are reported or
excluded explicitly; no intermediate point buffer returns to the CPU. The
repository contains only the runner and counts, not the production assets or
benchmark JSON:

```bash
NANOXGEN_CPU_BENCHMARK_REPEATS=3 \
./scripts/run_rabbit_luisa_benchmark.sh \
  /external/LuisaCompute-next/build-nanoxgen-hip/bin \
  /external/rabbit/yxt_rabbit__yxt_test_fur.xgen \
  /external/generated-rabbit-nxg
```

On 2026-07-22, the RX 9070 XT `gfx1201` run covered all 2,456,139 strands and
47,421,673 points. With three warm-ups, the seven-repeat HIP median/p90 was
`144.434/153.013 ms` (328.33 million points/s); the same partial plan's CPU
median was `1103.169 ms`, including output allocation, so HIP was 7.64x faster
than that CPU implementation. A separate 15-repeat run measured
`148.203/155.541 ms`, showing the expected run-to-run range. GPU allocation,
asset upload, and all per-description JIT compiles took 7.14 seconds on the
warm filesystem/driver run and 16.80 seconds on an earlier cold run.

The optional full CPU/GPU differential checked 189,686,692 final position and
radius components. Final radius matched within the printed float precision;
the combined RMS error was about `1e-6`, with 28,060 components above `1e-5`
and 189 above `1e-4`; the maximum was `0.001430`. The outliers are localized
in base guide interpolation, principally `erduo`, rather than the Luisa
postprocess: its root triangle identities were exact and root positions were
within `8e-6`, but spatial support-weight normalization near a support boundary
amplified that perturbation to `0.001619` before Cut reduced the maximum. This
is reported as a precision boundary, not hidden behind the small global RMS;
stable/scene-relative guide-blend acceptance still needs an Autodesk oracle.

These figures are deliberately labeled
`nanoxgen-native-partial-not-autodesk-equivalent`. Against the measured Maya
Classic evaluation/copy time of 155.119 seconds, the numerical ratios would be
about 140.6x for CPU and 1074x for hot HIP, but they are not valid final Maya
speedups. The native result does not yet reproduce the same positions/radii:
all nine descriptions retain fallback reasons for Autodesk root sampling and
authored NoiseFX; the asset also needs description-dependent ClumpingFX, PTEX
length/width/masks, and the first `erduo` Cut expression. Equality must be
established by counts, identities, channels, tolerances, and checksums before a
Maya speedup is claimed.
