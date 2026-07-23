# LuisaCompute `next` execution backend

NanoXGen can consume a separately built LuisaCompute `next` tree as an explicit
JIT execution backend. It is never downloaded, discovered, or linked by the
default CPU or Autodesk presets. LuisaCompute sources, submodules,
shader caches, runtime plug-ins, and build products must remain outside this
repository.

The current renderer/motion validation uses the published `next` revision
`ccef52f6f4defa26de07095dc6349cd314eb09b8`. The original HIP/Vulkan benchmark used upstream revision
`c07bc0824aebb97784cdaff6e2c34b77acab20a8`. Fallback ABI validation and the
correct packed-vector reader were retested after rebasing onto
`90bbb3b3155f9167dc2d332d95d7a7ffc0032014`. The `next` branch moves, so record
the exact revision used for every benchmark. The Luisa-side ABI documentation,
debug-only static fallback diagnostic, and unit coverage are included in the
current `next`; typed `float3` buffers retain their required 16-byte alignment,
while packed byte-buffer clients use scalar/array loads.

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

For a fallback-only build, including hosts where the moving `next` HIPRT
submodule does not yet compile for the installed ROCm/device combination:

```bash
NANOXGEN_LUISA_ENABLE_HIP=OFF \
NANOXGEN_LUISA_ENABLE_FALLBACK=ON \
./scripts/build_luisa_compute_next.sh \
  /external/LuisaCompute-next \
  /external/LuisaCompute-next/build-nanoxgen-fallback
```

`NANOXGEN_LUISA_ENABLE_HIP` defaults to `ON`. At least one of HIP or fallback
must be enabled.

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
allocation, JIT, upload, first dispatch/download/packing, total cold time,
warm dispatch statistics, and a checksum separately. Collection mode accepts
the one authored master `.xgen`, prepares all descriptions, compiles every
specialized kernel concurrently on one Device, and executes all descriptions
on that same Device and Stream. Each measured round is still a fresh process,
so it includes a real no-cache device/JIT cold start. No asset, cache, shader
artifact, or benchmark JSON is committed:

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
  --single-device-collection \
  --threads 0 \
  --rounds 5 --gpu-warmup 0 --gpu-repeats 1 --no-outer-warmup
```

`--threads 0` creates one `NanoXGenContext` sized from the process CPU affinity;
a positive value selects an explicit pool size. The benchmark reuses that
context across host preparation and JIT. Applications can instead construct
the context around a renderer-owned `TaskExecutor`.

Descriptions, PTEX sampling, clump binding, guide-runtime evaluation, and
kernel compilation submit work to a single recursive-safe dynamic queue.
Strand counts determine how many chunks are exposed, while physical concurrency
never exceeds the context size. This replaces the calibration-host-specific
four-description/eight-inner-worker split. Results are still written back in
authored description order regardless of completion order.

LLVM/HIP JIT is a separate resource profile from PTEX and clump work. For
contexts larger than four workers, a large compile batch defaults to roughly
75% as many simultaneous `Device::compile` lanes; the remaining workers are
not another pool. On the 32-worker Rabbit host, a fresh-process sweep measured
6.255 s at 8 workers, 5.365 s at 16, 5.185 s at 24, and 5.269 s at 32.
The host phase was effectively flat at 24–32 workers; the difference came from
LLVM allocator and memory-bandwidth contention during JIT.

After integrating the shared context, five fresh HIP processes using a
32-worker context and the adaptive 24-lane JIT policy measured 5.280 s median
and 5.309 s p90. Median host preparation was 2.719 s and median JIT was
1.965 s; all five collection checksums were
`10147028508645780229`. This is a different Luisa 0.9 `next` revision
(`ccef52f`) from the historical table below. Compared with that table's
four-by-eight host policy, host work improved by about 5.6%, while the newer
Luisa/HIP JIT regressed enough that total cold time is 1.6% slower. These
revision-separated figures are reported explicitly rather than attributing the
JIT change to NanoXGen scheduling.

### Cold Classic motion result

The motion benchmark uses the same one-Device/no-shader-cache boundary and
adds repeated `--motion-sample LOOKUP PLACEMENT` arguments. On 2026-07-24 a
repository-external Rabbit Alembic was made from the complete production mesh:
all four patches and `xgen_Pref` were retained, while only the
`xgen_eyelash` parent transform received a second sample translated by +2 on
X. This avoids a static-archive false positive without committing a generated
fixture.

```bash
python scripts/run_xgen_classic_collection_benchmark.py \
  --luisa-tool \
    ./build/luisa-classic-hip-release/nanoxgen_xgen_classic_luisa_benchmark \
  --luisa-runtime /external/LuisaCompute-next/build-hip/bin \
  --backend hip \
  --collection /external/rabbit/collection.xgen \
  --archive /external/rabbit/rabbit-motion.abc \
  --descriptions-root /external/rabbit/project-root \
  --single-device-collection --threads 0 \
  --motion-sample 0 0 --motion-sample 1 1 \
  --rounds 1 --gpu-warmup 0 --gpu-repeats 1 --no-outer-warmup
```

The deliberately project-level `descriptions-root` also exercises
`xgDataPath` relocation and mixed-separator `${DESC}` resolution. The fresh
HIP process produced 4,912,278 sample curves and 94,843,346 sample points,
reported ten unique description/deformation pairs (nine base deformations
plus the moving eyelash), and completed with zero fallback. All 25,738
eyelash renderer points moved, with maximum delta `2.00000381`; no other
NanoXGen point moved because static samples alias the base deformation.

One final post-indexing run measured 5.237 s cold end to end: 2.804 s native
preparation, 1.857 s parallel no-cache JIT, 0.058 s upload, and 0.322 s first
dispatch/download/packing. The Maya 2027.1 typed `PrimitiveCache` bridge on
the same two samples measured 286.185 s evaluation/copy and 288.976 s total
process wall. NanoXGen HIP was therefore 54.65x faster than typed evaluation
and 55.18x faster than summed process wall for this one-round, cold,
position/radius-only comparison. Maya reported the same 25,738 genuinely
moving eyelash points and `2.00000215` maximum delta; it also introduced
20,280 tiny `1e-6`-scale differences on nominally static descriptions, whereas
NanoXGen's deformation aliases kept those samples bit-identical.
A matching Luisa fallback run produced the same moving-point count and a
`2.00000334` maximum delta in 7.109 s cold; its larger costs were 2.927 s JIT
and 1.147 s first dispatch/download/packing.

This is a nonzero-motion correctness/performance calibration, not a
multi-round p90 claim and not evidence of native compatibility for arbitrary
Classic assets. The static five-round table below remains the more stable
backend comparison.

### Complete collection result

On 2026-07-23, the RX 9070 XT (`gfx1201`) processed all nine Rabbit
descriptions without Autodesk fallback. Every path produced 2,456,139 curves
and 47,421,673 renderer points with exact per-description canonical identities.
The HIP executable and plug-in came from LuisaCompute revision
`c07bc0824aebb97784cdaff6e2c34b77acab20a8`; fallback ABI and Vulkan were
retested against `90bbb3b3155f9167dc2d332d95d7a7ffc0032014`.

Each row below contains five fresh-process samples with no outer warm-up.
Within a GPU sample all nine descriptions share one Device; 80 specialized
kernels compile as one collection-wide batch using the machine's 32 logical
threads. Times include collection and sidecar file I/O and all native
preparation. GPU times also include device creation, allocation, upload, JIT,
first dispatch, download, and packing with the shader cache disabled. They
exclude cache writes and Autodesk serialization. Maya uses the direct typed
`PrimitiveCache` bridge, with no intermediate XGen BLOB; its process-wall
median was 158.590 s, while the comparable typed evaluation/copy interval
shown below was 156.225 s.

| path | first sample | median | p90 | speed vs Maya |
| --- | ---: | ---: | ---: | ---: |
| portable CPU Release | 11.519 s | 11.522 s | 11.558 s | 13.56x |
| native CPU + LTO | 11.370 s | 11.351 s | 11.403 s | 13.76x |
| Luisa HIP, one Device, no shader cache | 5.219 s | 5.199 s | 5.230 s | 30.05x |
| Luisa Vulkan, one Device, no shader cache | 6.519 s | 6.519 s | 6.654 s | 23.96x |
| Luisa fallback, one Device, no shader cache | 6.564 s | 6.564 s | 6.622 s | 23.80x |
| Maya 2027.1 typed evaluation/copy | 156.225 s | 156.225 s | 156.436 s | 1.00x |

HIP is 2.18x faster than native+LTO CPU, Vulkan is 1.74x faster, and fallback
is 1.73x faster for this cold no-cache workload. The median HIP breakdown is
2.879 s native preparation, 1.764 s JIT, 0.045 s device creation, 0.088 s
collection parsing, 0.034 s buffer allocation, 0.087 s upload, and 0.318 s
first dispatch/download/packing. Vulkan spends 2.882 s in native preparation
and 2.976 s in JIT. Fallback spends 2.875 s in native preparation, 2.367 s in
JIT, and 1.121 s in first dispatch/download/packing.

Fallback was also measured with an earlier conservative one-worker JIT limit:
its cold time was 24.776 s and JIT alone took 16.874 s. Removing that
backend-name special case and using the native 32-worker JIT default first
reduced cold time to 10.283 s; collection-wide host preparation then reduced it
to the 6.564 s median above. The complete change is 3.77x end to end, with the
same stable collection checksum in every round. The public pipeline does not
receive a backend name. CPU concurrency now comes from the supplied
`NanoXGenContext`, independently of the caller-supplied Device and backend.
Warm-dispatch numbers are recorded by the benchmark but are deliberately not
substituted for the requested cold result.

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

The original fallback crash was a NanoXGen ABI error, not permission for a
backend-wide unaligned load. Luisa `float3` has 16-byte alignment; NanoXGen
loaded the packed `RootSample::normal` at byte offset 12 as `float3`, causing
fallback LLVM to emit a correctly aligned `vmovaps` for an incorrectly aligned
address. Packed `Vec3`, guide records, and 12-byte CV arrays are now read as
`std::array<float, 3>` and explicitly assembled into a DSL `Float3`. Typed
`Buffer<float3>` paths remain unchanged.

LuisaCompute now documents this byte-buffer ABI and fallback reports a clear
JIT error when a statically known byte offset violates the accessed type's
alignment. Its unit test covers both a 16-byte-aligned `float3` and a packed
scalar array. NanoXGen's Luisa differential test passes in multi-threaded and
`LUISA_SINGLE_THREADING=1` fallback modes, and all nine Rabbit descriptions
complete. Rabbit topology and canonical identities exactly match Maya; the
same five descriptions meet the strict `1e-3` geometry threshold as HIP and
native CPU, while the known ill-conditioned Noise/Clump outliers remain. RADV
reports its Vulkan implementation as non-conformant, so the Vulkan number is
experimental and device/driver-specific.
