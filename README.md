# NanoXGen

[![CI](https://github.com/Mike-Leo-Smith/NanoXGen/actions/workflows/ci.yml/badge.svg)](https://github.com/Mike-Leo-Smith/NanoXGen/actions/workflows/ci.yml)

NanoXGen is an experimental, GPU-oriented procedural hair representation and
generator inspired by the relationship between NanoVDB and OpenVDB. It is not
an Autodesk product and does not reimplement a proprietary binary layout.

The current prototype provides:

- a pointer-free, versioned, relocatable `.nxg` blob;
- rest-area-weighted deterministic root sampling on triangle meshes;
- precomputed fixed-size guide stencils for bounded GPU work;
- root-relative guide interpolation with compact support weights;
- width taper and an XGen-compatible 3D gradient-noise core with verified
  magnitude, frequency, correlation, and length preservation plus a
  parallel-transported surface frame;
- one shared C++/CUDA generation function and a CUDA launch kernel;
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
- optional native ISA, SIMD-width, IPO/LTO, and precision-gated fast-math modes.

## Build and run without CMake

```bash
make test
make bin/nanoxgen_demo
./bin/nanoxgen_demo build/demo
```

The demo writes `build/demo/demo.nxg` and `build/demo/curves.obj`.

## SDK setup

See [`docs/sdk-setup.md`](docs/sdk-setup.md). The official Maya DevKit is
downloadable without sign-in, but Autodesk ships the actual XGen headers and
`libAdskXGen` only with a full Maya installation.

With a licensed full Maya installation, run the real XGen suite on Linux:

```bash
export MAYA_LOCATION=/usr/autodesk/maya2027
./scripts/run_xgen_real_tests.sh
```

The suite creates deterministic Interactive Grooming scenes with Maya
Standalone, exports real `outRenderData` BLOBs, loads them through Autodesk's
`XgFnSpline`, validates all spline ranges and numeric arrays, canonicalizes
modifier-reordered curves by face/root coordinates, and performs strict
per-attribute comparisons. The complex case contains 244 nine-CV strands on a
wave surface with cut and width taper. Maya and its generated BLOBs remain
external to this repository.

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

`nanoxgen_xgen_cache` converts already-evaluated official spline BLOBs to the
runtime-only `.nxc` cache. `--renderer-minimal` stores only the topology and
bit-exact renderer points; `--motion` adds separately evaluated shutter samples
after canonical face/UV matching. The converter is built only when a licensed
XGen SDK is available; loading `.nxc` does not require Autodesk libraries.

For production compiler experiments, run `make fast-math-check`. The test
compares every float from strict and fast builds and fails if its documented
relaxed error budget is exceeded.

## Research status

See [`docs/xgen-research.md`](docs/xgen-research.md) for the verified XGen model,
documented interpolation algorithm, current approximations, and clean-room plan.
See [`docs/data-layout.md`](docs/data-layout.md) for the GPU blob layout.
See [`docs/renderer-integration.md`](docs/renderer-integration.md) for the exact
curve payload contract and coverage relative to the Maya renderer paths.

## Continuous integration

GitHub Actions builds the CPU library and demo with CMake and runs the test
suite on every push and pull request. The optional Autodesk XGen probe and CUDA
kernel are excluded from hosted CI because they require external SDK/toolchain
installations. Run `scripts/run_xgen_real_tests.sh` on a licensed Maya machine
for the proprietary-runtime integration coverage.
