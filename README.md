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
- one shared C++/CUDA generation function and a CUDA launch kernel;
- an executable CPU/CUDA parity suite plus a GPU-resident JSON benchmark;
- direct CPU/CUDA generation into renderer `float4(position, radius)` and
  fixed `pointCounts` buffers, with checked device capacities and deformation
  lengths at the public CUDA boundary;
- persistent CPU workers that claim CUDA-block-sized strand tiles through an
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
- a self-contained v1 Interactive Grooming `.xgen` BLOB parser, lossless
  container round-trip, curve processor, writer, inspector, and `.nxc`
  converter using only C++ and zlib;
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

Use `debug`, `native-release`, or `cuda-release` in place of `release` for the
corresponding core configuration. CUDA builds require an installed CUDA
toolkit; the CUDA test is reported as skipped when the build host has NVCC but
no usable GPU. None of these presets discovers or links Maya/XGen.

The Makefile is a fallback for minimal CPU-only environments:

```bash
make test
make bin/nanoxgen_demo
./bin/nanoxgen_demo build/demo
```

The demo writes `build/demo/demo.nxg` and `build/demo/curves.obj`.

## Autodesk calibration (optional)

The NanoXGen core loads, validates, processes, and writes Interactive Grooming
`outRenderData` `.xgen` BLOBs, `.nxg` assets, and `.nxc` caches without Maya or
any Autodesk library. This is the binary Interactive Grooming container, not
the unrelated classic XGen text-description format that also uses `.xgen`.

```bash
./build/release/nanoxgen_xgen_inspect groom.xgen
./build/release/nanoxgen_xgen_process groom.xgen scaled.xgen --length-scale 0.8
./build/release/nanoxgen_xgen_cache groom.xgen groom.nxc
```

The demo also writes a new `.xgen` directly from NanoXGen procedural output.
See [`docs/xgen-format.md`](docs/xgen-format.md) for the container contract.

See [`docs/sdk-setup.md`](docs/sdk-setup.md). The official Maya DevKit is
downloadable without sign-in, but Autodesk ships the actual XGen headers and
`libAdskXGen` only with a full Maya installation.

With a licensed full Maya installation, run the real XGen suite on Linux:

```bash
export MAYA_LOCATION=/usr/autodesk/maya2027
./scripts/run_xgen_real_tests.sh
```

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
