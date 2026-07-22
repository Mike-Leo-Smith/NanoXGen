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
- width taper and a first strand-stable noise modifier;
- one shared C++/CUDA generation function and a CUDA launch kernel;
- persistent CPU workers that claim CUDA-block-sized strand tiles through an
  atomic counter;
- validation, corruption detection, tests, and an OBJ curve preview;
- a Maya 2027.1-tested Autodesk XGen Interactive Grooming probe and real-fixture
  differential harness.

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
`XgFnSpline`, validates all spline ranges and numeric arrays, and compares
independent exports by canonical geometry hash. Maya and its generated BLOBs
remain external to this repository.

## Research status

See [`docs/xgen-research.md`](docs/xgen-research.md) for the verified XGen model,
documented interpolation algorithm, current approximations, and clean-room plan.
See [`docs/data-layout.md`](docs/data-layout.md) for the GPU blob layout.

## Continuous integration

GitHub Actions builds the CPU library and demo with CMake and runs the test
suite on every push and pull request. The optional Autodesk XGen probe and CUDA
kernel are excluded from hosted CI because they require external SDK/toolchain
installations. Run `scripts/run_xgen_real_tests.sh` on a licensed Maya machine
for the proprietary-runtime integration coverage.
