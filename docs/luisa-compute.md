# LuisaCompute `next` execution backend

NanoXGen can consume a separately built LuisaCompute `next` tree as an explicit
JIT execution backend. It is never downloaded, discovered, or linked by the
default CPU or Autodesk presets. LuisaCompute sources, submodules,
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
points and root records through LuisaCompute and executes generation and the
Classic float runtime without a host copy between stages. The production
Classic path additionally uploads exact root identities, SeExpr random
prefixes, surface tangents, rebuilt guides, and CSR guide associations.

The runtime boundary is deliberately typed: `XgenFloatExpressionProgram`
contains float immediates and the Luisa lowering accepts only that program
type. Device contexts, inputs, expression values, hashes, and random values use
only `float` and `uint32`. The strict Autodesk/SeExpr calibration evaluator is
a separate CPU path that retains `double`, because replacing it would make the
oracle itself inaccurate. It is converted once to the compact float runtime IR
and is never captured by a Luisa callable or kernel.

The device hash reconstructs SeExpr's float-input component with integer
arithmetic and appends the exact CPU-planned `(u,v,faceSeed)` prefix. It uses
only `float`, `uint32`, and `uint64`; no FP64 instruction is required. On the
RX 9070 XT test, HIP and Vulkan expression results stayed within `5.96e-8` of
the CPU float evaluator.

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
This is not yet a claim that every Classic XGen description is natively
evaluable. Unsupported SeExpr, PTEX-bound attributes, modules, and topology
operations remain checked errors and select the Autodesk fallback until their
CPU oracle and Luisa differential tests pass.

## Rabbit cold/no-cache benchmark

`nanoxgen_xgen_classic_luisa_benchmark` starts from the external Classic
collection, Alembic patch and PTEX maps. It disables the Luisa shader cache and
reports device creation, native parse/import/root/rebuild, allocation/JIT,
upload, first dispatch/download/packing, total cold time, warm median/p90 and a
checksum separately. No asset or benchmark JSON is written to the repository:

```bash
export LUISA_COMPUTE_SOURCE_DIR=/external/LuisaCompute-next
export LUISA_COMPUTE_BUILD_DIR=/external/LuisaCompute-next/build-nanoxgen-hip
cmake --preset luisa-classic-hip-release
cmake --build --preset luisa-classic-hip-release

./build/luisa-classic-hip-release/nanoxgen_xgen_classic_luisa_benchmark \
  "$LUISA_COMPUTE_BUILD_DIR/bin" hip \
  /external/rabbit/collection.xgen /external/rabbit/patches.abc \
  /external/rabbit/xgen/collections/collection eyelash \
  --warmup 3 --repeats 11 --reference-nxc /external/oracle/eyelash.nxc
```

On 2026-07-23, Rabbit `eyelash` on the RX 9070 XT (`gfx1201`) produced the same
1514 curves and 25738 renderer points as Maya, with `fallback_count=0`. A fresh
no-shader-cache run measured 890.7 ms cold and 0.117 ms warm-median through HIP;
Vulkan measured 746.5 ms cold and 0.360 ms warm-median. The HIP maximum
position/radius errors against the Maya `.nxc` oracle were about `1.59e-4` and
`5.59e-9`. Strict HIP math reduced the CPU differential but increased cold JIT
to about 3.15 s, so fast-math is the practical GPU mode for this float pipeline.

Eleven measured processes after three warmups gave portable CPU and native+LTO
end-to-end medians of 92.34 and 90.79 ms (p90 93.39 and 91.49 ms). The matching
Maya typed evaluation/copy median was 259.81 ms (p90 263.85 ms). Thus portable
CPU is 2.81x and native+LTO CPU is 2.86x faster than Maya for this description.
Cold HIP and Vulkan are 3.43x and 2.87x slower than Maya because JIT consumes
748 and 641 ms respectively. Warm GPU dispatch is intentionally not presented
as the requested cold speedup.

After matching the internal Noise coordinate unit and Autodesk's fixed
`SgCurve::length` approximation, a fresh no-cache HIP run of Rabbit `head_A`
processed 307791 curves / 5232447 renderer points with all four effects and
`fallback_count=0`. It measured 4784.1 ms cold, including 4304.1 ms allocation
and JIT, and 10.78 ms warm median. HIP differed from the native CPU path by at
most `1.78e-3`; both reached about `3.16e-3` maximum position error against the
Maya oracle because of the same subdivision-boundary outlier. Thus `head_A`
still fails the strict `1e-3` maximum-error gate even though the CPU RMS error is
about `4.13e-6`. Full Rabbit remains incomplete: seven descriptions have
explicit ClumpingFX/PTEX/expression fallbacks. A zero lowering-fallback count is
reported separately from `oracle_within_tolerance`.

The Luisa `fallback` backend built against system LLVM 22/Embree 4.4.1 on this
Arch host but crashed in its generated worker code even for the small parity
test, including with `LUISA_SINGLE_THREADING=1`. HIP and Vulkan are the tested
working backends; the fallback crash occurs below NanoXGen and remains an
upstream/runtime compatibility issue.

Rabbit `mm` now exercises the Luisa hierarchical ClumpingFX kernel as well as
NoiseFX, CutFX, and width. A fresh no-cache run produced 3526 curves / 59942
renderer points with `fallback_count=0`. HIP differed from the CPU float path
by at most `1.72e-5`; Vulkan differed by at most `1.49e-4`. Their Maya-oracle
maximum remained `2.62e-3`, inherited from the CPU clump approximation, so the
strict `1e-3` oracle gate correctly remains false.

Cold startup is currently the wrong tradeoff for this small description. HIP
took 6728.7 ms end to end, including 6489.4 ms for allocation/JIT, and Vulkan
took 5601.2 ms, including 5385.4 ms for allocation/JIT. Warm execution was
0.380 ms and 0.752 ms respectively. The matching portable CPU cold path took
223.6 ms, while Maya typed evaluation/copy took 334.6 ms. Thus CPU was about
1.50x faster than Maya, but no-cache HIP and Vulkan were about 20.1x and 16.7x
slower. These results make shader fusion/precompiled backend artifacts—not
curve dispatch throughput—the next cold-start optimization target.
