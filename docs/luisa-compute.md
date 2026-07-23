# LuisaCompute `next` execution backend

NanoXGen can consume a separately built LuisaCompute `next` tree as an explicit
JIT execution backend. It is never downloaded, discovered, or linked by the
default CPU or Autodesk presets. LuisaCompute sources, submodules,
shader caches, runtime plug-ins, and build products must remain outside this
repository.

The tested upstream revision is
`c07bc0824aebb97784cdaff6e2c34b77acab20a8`. The `next` branch moves, so record
the exact revision used for every benchmark.

That revision needs the checked-in fallback byte-buffer alignment fix on LLVM
22. Apply it before configuring a build that enables `fallback`:

```bash
git -C /external/LuisaCompute-next apply --ignore-space-change \
  /path/to/NanoXGen/patches/luisa-compute-next-fallback-unaligned-byte-buffer.patch
```

The patch also adds its LuisaCompute unit regression. It is intentionally kept
as a source patch because LuisaCompute remains an external dependency. The
`--ignore-space-change` option handles the mixed CRLF/LF line endings already
present in the upstream fallback source; it does not weaken the patched
revision or hunk checks.

## AMD HIP build

Clone recursively outside NanoXGen, then use the checked-in helper to configure
only the WIP HIP backend:

```bash
git clone --branch next --recursive \
  https://github.com/LuisaGroup/LuisaCompute.git \
  /external/LuisaCompute-next

NANOXGEN_HIP_ARCH=gfx1201 \
NANOXGEN_LUISA_ENABLE_FALLBACK=ON \
./scripts/build_luisa_compute_next.sh \
  /external/LuisaCompute-next \
  /external/LuisaCompute-next/build-nanoxgen-hip
```

`NANOXGEN_LUISA_ENABLE_FALLBACK` defaults to `OFF`; setting it to `ON` builds
the fallback plug-in alongside HIP. The same NanoXGen binaries can then select
either backend at runtime.

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
evaluable. Runtime PTEX `map()` attributes and no-argument palette scalar
functions are pre-evaluated into float buffers. Unsupported vector SeExpr,
modules, and topology operations remain checked errors and select the Autodesk
fallback until their CPU oracle and Luisa differential tests pass.

## Rabbit cold/no-cache benchmark

`nanoxgen_xgen_classic_luisa_benchmark` starts from an external Classic
collection, Alembic patch, XPD guides, and PTEX maps. It disables the Luisa
shader cache and reports device creation, native parse/import/root/rebuild,
allocation/JIT, upload, first dispatch/download/packing, total cold time,
warm dispatch statistics, and a checksum separately. The collection driver
runs every description in an isolated process and aggregates identical rounds.
No asset, cache, shader artifact, or benchmark JSON is committed:

```bash
export LUISA_COMPUTE_SOURCE_DIR=/external/LuisaCompute-next
export LUISA_COMPUTE_BUILD_DIR=/external/LuisaCompute-next/build-nanoxgen-hip
cmake --preset luisa-classic-hip-release
cmake --build --preset luisa-classic-hip-release

python scripts/run_xgen_classic_collection_benchmark.py \
  --luisa-tool \
    ./build/luisa-classic-hip-release/nanoxgen_xgen_classic_luisa_benchmark \
  --luisa-runtime "$LUISA_COMPUTE_BUILD_DIR/bin" \
  --backend hip \
  --collection /external/rabbit/collection.xgen \
  --archive /external/rabbit/patches.abc \
  --descriptions-root /external/rabbit/xgen/collections/collection \
  --descriptions mm erduo body eyelash head nose sizhi weiba head_A \
  --rounds 5 --gpu-warmup 3 --gpu-repeats 11 --no-outer-warmup
```

### Complete collection result

On 2026-07-23, the RX 9070 XT (`gfx1201`) processed all nine Rabbit
descriptions without Autodesk fallback. Every path produced 2,456,139 curves
and 47,421,673 renderer points with exact per-description canonical identities.
The GPU executable and runtime plug-ins came from the same LuisaCompute
revision, `c07bc0824aebb97784cdaff6e2c34b77acab20a8`.

Each row below contains five fresh process-isolated samples with no outer
warm-up. Times include collection and sidecar file I/O and all native
preparation. GPU times also include device creation, upload, JIT, first
dispatch, download, and packing with the shader cache disabled. They exclude
cache writes and Autodesk serialization. Maya uses the direct typed
`PrimitiveCache` bridge, with no intermediate XGen BLOB; its process-wall
median was 158.590 s, while the comparable typed evaluation/copy interval
shown below was 156.225 s.

| path | first sample | median | p90 | speed vs Maya |
| --- | ---: | ---: | ---: | ---: |
| portable CPU Release | 37.416 s | 37.246 s | 37.416 s | 4.19x |
| native CPU + LTO | 37.699 s | 36.826 s | 37.774 s | 4.24x |
| Luisa HIP, no shader cache | 17.822 s | 17.658 s | 17.850 s | 8.85x |
| Luisa Vulkan, no shader cache | 20.996 s | 20.958 s | 20.996 s | 7.45x |
| Luisa fallback, no shader cache | 28.474 s | 27.996 s | 28.474 s | 5.58x |
| Maya 2027.1 typed evaluation/copy | 156.225 s | 156.225 s | 156.436 s | 1.00x |

HIP is 2.09x faster than native+LTO CPU, and Vulkan is 1.76x faster, for this
cold no-cache workload. Fallback is 1.32x faster than native+LTO CPU;
HIP is 1.59x faster than fallback, and Vulkan is 1.34x faster than fallback.
The median HIP breakdown is 10.513 s native
parse/import/root/rebuild, 6.316 s JIT/allocation, 0.413 s device creation,
0.069 s upload, and 0.340 s first dispatch/download/packing. Vulkan spends
10.517 s in native preparation and 9.685 s in JIT/allocation, which explains
most of its gap to HIP. Fallback spends 10.444 s in native preparation,
16.281 s in JIT/allocation, 0.042 s in device creation, 0.085 s in upload, and
1.132 s in first dispatch/download/packing. Warm-dispatch numbers are recorded
by the benchmark but are deliberately not substituted for the requested cold
result.

The Luisa kernels use `float`, integer identities, and integer hashes. No
handwritten CUDA/HIP path remains and no device-side `double` is used; the
host-only double code exists to reproduce Autodesk calibration behavior.
Backend checksums are stable across all five rounds. HIP and Vulkan checksums
are not expected to match bit-for-bit because the backends may contract or
reassociate float operations differently.

### Oracle boundary

The full collection has exact topology and canonical identities, but not every
point passes the strict geometry tolerance. Against fresh Maya `.nxc` typed
caches, five descriptions have maximum position error at or below `1e-3`.
Across all descriptions, 1,479 of 47,421,673 points exceed `1e-3` (0.00312%),
the point-weighted RMS is about `6.39e-5`, and the worst rare outlier is
`0.243994`. These outliers occur where tiny float/double differences are
amplified by ill-conditioned Clump/Noise transported frames. See
`classic-native.md` for the per-description table. Therefore the timing table
is a same-topology, same-channel engineering comparison for this Rabbit asset,
not a bit-exact geometry claim or a native-support claim for arbitrary XGen.

The original Luisa `fallback` backend generated an LLVM `vmovaps` for a
`float3` loaded at byte offset 12. Byte buffers and bindless byte-buffer views
do not promise the natural 16-byte `float3` alignment, so the aligned load
faulted in the fallback worker. The checked-in patch marks only byte-addressed
loads and stores as alignment 1; ordinary typed buffers retain their natural
alignment. Optimized LLVM IR was audited for the direct and bindless
regressions, and all four loads/stores use `align 1`.

The patched backend passes LuisaCompute's `test_byte_buffer` with both direct
and bindless unaligned `float3` access, NanoXGen's Luisa differential test in
multi-threaded and `LUISA_SINGLE_THREADING=1` modes, and all nine Rabbit
descriptions. Rabbit topology and canonical identities exactly match Maya;
the same five descriptions meet the strict `1e-3` geometry threshold as HIP
and native CPU, while the known ill-conditioned Noise/Clump outliers remain.
RADV reports its Vulkan implementation as non-conformant, so the Vulkan number
is experimental and device/driver-specific.
