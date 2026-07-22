# NanoXGen

[![CI](https://github.com/Mike-Leo-Smith/NanoXGen/actions/workflows/ci.yml/badge.svg)](https://github.com/Mike-Leo-Smith/NanoXGen/actions/workflows/ci.yml)

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
- an explicit LuisaCompute `next` JIT integration, tested through HIP and
  Vulkan on AMD, without adding it to default core dependencies or retaining
  handwritten CUDA/HIP kernels;
- a bounded scalar Classic-expression SSA compiler with a strict CPU
  Autodesk/SeExpr calibration mode and a separate float-only Luisa runtime IR;
- executable CPU/GPU parity suites plus GPU-resident JSON benchmarks;
- direct CPU and Luisa generation into renderer `float4(position, radius)` and
  fixed `pointCounts` buffers, with checked device capacities at the public
  GPU boundary;
- persistent CPU workers that claim fixed-size strand tiles through an
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

Use `debug` or `native-release` in place of `release` for the corresponding
core configuration. The default CPU presets do not discover or link
LuisaCompute, ROCm, Maya, or XGen; the optional Luisa preset does not discover
or link Maya/XGen.

LuisaCompute is a separate optional execution backend. Its moving `next`
checkout and build stay outside this repository; see
[`docs/luisa-compute.md`](docs/luisa-compute.md) for the tested revision, AMD
HIP/Vulkan build, cache behavior, and explicit `luisa-hip-release` and
`luisa-classic-hip-release` presets.

The Classic Alembic/OpenSubdiv/Ptex input stage is also explicit and optional. See
[`docs/classic-native.md`](docs/classic-native.md) for its
`classic-alembic-release` preset, external-asset command, and current
subdivision, PTEX binding, and modifier parity boundary.

The combined Luisa/Classic preset builds a no-shader-cache cold benchmark from
the authoring collection, Alembic patch, PTEX density, exact roots and guide
associations through the final renderer points. The Rabbit `eyelash`
description currently completes this path with no Autodesk fallback, matches
Maya's 1514-curve/25738-point topology, and passes the recorded geometry
tolerance. `head_A` also runs all four Noise/Cut passes with no fallback and
matches its 307791-curve/5232447-point topology. Its CPU result has about
`4.13e-6` position RMS error against Maya, but a subdivision-boundary strand
reaches `3.16e-3`, so it still fails the strict `1e-3` maximum-error gate.
Classic ClumpingFX is also decoded from XPD3 point records plus its PTEX
guide-ID map and runs in the CPU float runtime. The runtime supports ordered
clump hierarchies, guide-space clump noise, and control grouping already baked
into those maps. Rabbit `mm` now evaluates both ClumpingFX modules, Noise2, and
Cut1 with zero CPU fallback and matches Maya's 3526-curve/59942-point topology;
the full result has `2.62e-3` maximum and `1.23e-4` RMS position error. Other
advanced clump controls, the Luisa clump pass, and PTEX-expression bindings
remain explicit fallbacks, so the complete Rabbit package is not yet claimed
as native-compatible.

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

The default `release`, `debug`, and `native-release` presets neither find nor
link LuisaCompute, Maya, or XGen. Classic authoring packages, Interactive Maya
authoring graphs, evaluated `XgSplineData` renderer BLOBs, and NanoXGen `.nxc`
runtime caches are four distinct layers; see the production-assets document
for their support boundaries.

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

GitHub Actions configures CMake/Ninja presets, builds the portable CPU targets,
and runs the core test suite on every push and pull request. LuisaCompute device
tests require an explicitly supplied external `next` build and supported GPU.
Autodesk calibration is intentionally excluded; run
`scripts/run_xgen_real_tests.sh` on a licensed Maya machine for that coverage.
