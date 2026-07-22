# NanoXGen

[![CI](https://github.com/Mike-Leo-Smith/NanoXGen/actions/workflows/ci.yml/badge.svg)](https://github.com/Mike-Leo-Smith/NanoXGen/actions/workflows/ci.yml)
[![CUDA](https://github.com/Mike-Leo-Smith/NanoXGen/actions/workflows/cuda.yml/badge.svg)](https://github.com/Mike-Leo-Smith/NanoXGen/actions/workflows/cuda.yml)

NanoXGen is an experimental, GPU-oriented procedural hair representation and
generator inspired by the relationship between NanoVDB and OpenVDB. It is not
an Autodesk product. Its Interactive Grooming BLOB support is an independent
implementation and does not link or redistribute Autodesk code.

The current prototype provides:

- a pointer-free, versioned, relocatable `.nxg` blob;
- rest-area-weighted deterministic root sampling on triangle meshes;
- precomputed fixed-size guide stencils for bounded GPU work;
- root-relative guide interpolation with compact support weights;
- width taper and an XGen-compatible 3D gradient-noise core with verified
  magnitude, frequency, correlation, and length preservation plus a
  parallel-transported surface frame;
- one shared C++/CUDA/HIP generation function with CUDA and AMD HIP launch
  kernels;
- an explicit LuisaCompute `next` JIT integration, tested through its WIP HIP
  backend on AMD, without adding it to default core dependencies;
- a bounded scalar Classic-expression SSA compiler with a strict CPU
  Autodesk/SeExpr calibration mode and a separate float-only Luisa runtime IR;
- executable CPU/GPU parity suites plus GPU-resident JSON benchmarks;
- direct CPU/CUDA/HIP generation into renderer `float4(position, radius)` and
  fixed `pointCounts` buffers, with checked device capacities and deformation
  lengths at the public GPU boundary;
- persistent CPU workers that claim GPU-block-sized strand tiles through an
  atomic counter;
- validation, corruption detection, tests, and an OBJ curve preview;
- a Maya 2027.1-tested Autodesk XGen Interactive Grooming probe and real-fixture
  differential harness;
- a linear modifier reference path with strict XGen base/cut/taper checks on a
  subdivided wave surface (not a complete generation benchmark);
- reproducible in-process NanoXGen and real XGen performance benchmarks;
- a renderer-facing curve payload with radius conversion, motion samples,
  root UV/custom primvars, affine transforms, resampling, and 64K batching;
- frame-local deformed mesh/normal/guide overlays with stable root identities;
- a bit-exact evaluated-curve `.nxc` cache with canonical motion matching, for
  bypassing Maya/XGen on repeated static renders;
- a self-contained v1 Interactive Grooming `XgSplineData` BLOB parser, lossless
  container round-trip, curve processor, writer, inspector, and `.nxc`
  converter using only C++ and zlib;
- a production-package inventory that distinguishes Classic collections from
  evaluated BLOBs by content, resolves path variables, reports missing/external
  dependencies, refuses symlink traversal, and emits a typed manifest plus a
  native, Classic typed, or Interactive Maya backend execution plan;
- a bounded, Autodesk-free Classic collection parser with typed descriptions,
  ordered modules, bindings, patch faces, and packed embedded-guide data;
- an optional system-Alembic Classic input stage that imports selected patch
  faces and embedded guides into validated `.nxg` assets without Maya/XGen;
- an optional Classic curve bridge that consumes public XGen RenderAPI typed
  callbacks directly and can write `.nxc` without an intermediate renderer BLOB;
- an optional Interactive Maya command that serializes `outRenderData` to
  memory once, then builds source-order or exact-identity canonical `.nxc`
  without a temporary BLOB or `XgFnSpline` reload;
- fused source-order and canonical renderer-minimal ingestion that selectively
  retains only required arrays, plus a staged resident/hot-file benchmark;
- optional native ISA, SIMD-width, IPO/LTO, and precision-gated fast-math modes.

## Build and test

CMake with Ninja is the primary build path. The checked-in presets keep local
and CI configuration identical:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build/release --prefix /desired/prefix
```

Installed consumers can use `find_package(NanoXGen CONFIG REQUIRED)` and link
`NanoXGen::nanoxgen`; CI builds a separate downstream project to verify that
export on every change.

Use `debug`, `native-release`, `cuda-release`, or `hip-release` in place of
`release` for the corresponding core configuration. CUDA builds require an
installed CUDA toolkit. HIP builds require ROCm and may need an explicit local
compiler/architecture selection, for example:

```bash
cmake --preset hip-release \
  -DCMAKE_HIP_COMPILER=/opt/rocm/lib/llvm/bin/clang++ \
  -DCMAKE_HIP_ARCHITECTURES=gfx1201
cmake --build --preset hip-release
ctest --preset hip-release
```

GPU tests use CTest's skip code when a toolkit is available but no matching
device is usable. The default CPU presets do not discover or link CUDA, HIP,
Maya, or XGen; the optional GPU presets do not discover or link Maya/XGen.

LuisaCompute is a separate optional execution backend. Its moving `next`
checkout and build stay outside this repository; see
[`docs/luisa-compute.md`](docs/luisa-compute.md) for the tested revision, AMD
HIP build, cache behavior, and explicit `luisa-hip-release` preset.

The Classic Alembic input stage is also explicit and optional. See
[`docs/classic-native.md`](docs/classic-native.md) for its
`classic-alembic-release` preset, external-asset command, and current
subdivision/PTEX/modifier parity boundary.

The HIP build also provides direct generation and evaluated `.nxc` residency
benchmarks. The latter accepts multiple caches, so a complete multi-description
asset can be measured as one upload and validation workload:

```bash
./build/hip-release/nanoxgen_hip_benchmark groom.nxg \
  --strands 100000 --cvs 12 --noise
./build/hip-release/nanoxgen_hip_cache_benchmark \
  --warmup 3 --repeats 15 /external/rabbit-cache/*.nxc
```

The cache benchmark reports CPU read/validation, one-time host concatenation,
host-to-device transfer, and the GPU full-buffer validation/checksum separately.
It does not claim that HIP accelerates Autodesk Classic description evaluation.

The Luisa/HIP preset additionally builds a zero-host-copy Classic float-plan
benchmark. `scripts/run_rabbit_luisa_benchmark.sh` runs all nine descriptions
of the external Rabbit example as one warm batch and reports GPU and CPU
timings, JIT/setup cost, checksum, and every remaining fallback count. Its
output is explicitly partial-semantics until Autodesk-equivalent root sampling,
PTEX, clumping, and authored noise parity are complete.

The Makefile is a fallback for minimal CPU-only environments:

```bash
make test
make bin/nanoxgen_demo
./bin/nanoxgen_demo build/demo
```

The demo writes `build/demo/demo.nxg` and `build/demo/curves.obj`.

## Autodesk calibration (optional)

The NanoXGen core loads, validates, processes, and writes Interactive Grooming
`outRenderData` BLOBs, `.nxg` assets, and `.nxc` caches without Maya or any
Autodesk library. The calibration scripts historically give those BLOBs an
`.xgen` suffix, but they are evaluated renderer snapshots, not complete XGen
authoring assets. Classic XGen also uses `.xgen` for a text collection and a
production asset normally includes many sidecars.

```bash
./build/release/nanoxgen_xgen_inspect groom.xgen
./build/release/nanoxgen_xgen_process groom.xgen scaled.xgen --length-scale 0.8
./build/release/nanoxgen_xgen_cache groom.xgen groom.nxc
./build/release/nanoxgen_xgen_read_benchmark groom.xgen
./build/release/nanoxgen_xgen_package --require-complete /show/asset/xgen
./build/release/nanoxgen_xgen_classic_inspect \
  --description fur /show/asset/collection.xgen
```

The demo also writes a new `.xgen` directly from NanoXGen procedural output.
See [`docs/xgen-format.md`](docs/xgen-format.md) for the container contract.
See [`docs/xgen-production-assets.md`](docs/xgen-production-assets.md) for the
package boundary, dependency scanner, and Autodesk fallback architecture.
The Classic inspector validates collection structure and reports the exact
generation requirements it finds. It does not claim that every reported FX or
expression is natively evaluable yet.

See [`docs/sdk-setup.md`](docs/sdk-setup.md). The official Maya DevKit is
downloadable without sign-in, but Autodesk ships the actual XGen headers and
`libAdskXGen` only with a full Maya installation.

With a licensed full Maya installation, run the real XGen suite on Linux:

```bash
export MAYA_LOCATION=/usr/autodesk/maya2027
./scripts/run_xgen_real_tests.sh
```

Classic collection evaluation is a separate, explicitly Autodesk-linked path:

```bash
cmake --preset autodesk-bridge-release -DXGEN_ROOT="$XGEN_ROOT"
cmake --build --preset autodesk-bridge-release
./scripts/run_xgen_classic_typed_test.sh --help
./scripts/run_maya_xgen_cache_test.sh
```

The default `release`, `debug`, `native-release`, CUDA, and HIP presets neither
find nor link Maya/XGen. Classic authoring packages, Interactive Maya authoring
graphs, evaluated `XgSplineData` renderer BLOBs, and NanoXGen `.nxc` runtime
caches are four distinct layers; see the production-assets document for their
support boundaries.

The suite uses Maya only to create official fixtures and provide an oracle. It
compares NanoXGen's independent parser hash with `XgFnSpline`, requires the
standalone `.xgen`-to-`.nxc` cache to be byte-identical to the oracle cache,
and verifies that Autodesk can load NanoXGen-written BLOBs. The complex case
contains 244 nine-CV strands on a wave surface with cut and width taper. Maya
and its generated BLOBs remain external to this repository.

Run the clean-room modifier-identification matrix separately:

```bash
./scripts/run_xgen_modifier_study.sh
```

It records the public Maya schemas for noise, cut, clump, coil, and collision
nodes, then sweeps noise magnitude, frequency, correlation, length preservation,
and noise/cut order. A differential probe reports root drift, per-CV
displacement, tangent/normal components, arc-length change, width change, and
spatial correlation. A separate numerical verifier reconstructs the official
noise field from the independently implemented model and fails on a mismatch.
Generated Autodesk BLOBs stay under ignored build output.

Run the benchmark separately:

```bash
./scripts/run_xgen_benchmark.sh
```

It reports complete NanoXGen generation, renderer payload packing, the narrow
linear modifier reference, Maya BLOB export, and `XgFnSpline`
load/execute/materialization as separate stages. These stages are not collapsed
into one misleading speedup number.

The default release preset builds `nanoxgen_xgen_cache`. `--renderer-minimal`
stores only topology and bit-exact renderer points; `--motion` adds separately
evaluated shutter samples after canonical face/UV matching. The calibration
preset builds a separate `nanoxgen_xgen_cache_oracle` for differential tests.

For production compiler experiments, run `scripts/run_fast_math_comparison.sh`.
The test
compares every float from strict and fast builds and fails if its documented
relaxed error budget is exceeded.

## Research status

See [`docs/xgen-research.md`](docs/xgen-research.md) for the verified XGen model,
documented interpolation algorithm, current approximations, and clean-room plan.
See [`docs/data-layout.md`](docs/data-layout.md) for the GPU blob layout.
See [`docs/renderer-integration.md`](docs/renderer-integration.md) for the exact
curve payload contract and coverage relative to the Maya renderer paths.

## Continuous integration

GitHub Actions configures CMake/Ninja presets, builds the CPU targets, and runs
the core test suite on every push and pull request. A separate official CUDA
toolkit image compiles all CUDA targets and marks runtime parity as skipped when
no GPU is exposed. Autodesk calibration is intentionally excluded; run
`scripts/run_xgen_real_tests.sh` on a licensed Maya machine for that coverage.
