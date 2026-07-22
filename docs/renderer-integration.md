# Renderer integration contract

This document maps NanoXGen to the curve payload consumed by a production Maya
renderer integration. It covers both classic `xgmDescription` data received
through `XGenRenderAPI::PrimitiveCache` and Interactive Grooming data received
through `XGenSplineAPI::XgFnSpline`.

These Maya/XGen APIs describe calibration and one-time interchange paths, not
the NanoXGen runtime dependency graph. The core renderer path consumes `.nxg`
or `.nxc` and does not link Autodesk libraries. Raw Interactive Grooming
`outRenderData` `.xgen` v1 BLOBs are decoded by the standalone NanoXGen core;
Autodesk's API is retained only as a calibration oracle.

## Data the renderer actually consumes

The renderer-facing representation is much smaller than the Maya/XGen authoring
model:

1. one point count per curve;
2. one `float4(position, radius)` per CV;
3. optional absolute point positions for a second shutter sample;
4. optional face-uniform root UVs;
5. optional face-uniform float and color primvars.

XGen reports full widths. The renderer stores half-width/radius in the fourth
point component. `build_curve_batches` performs that conversion explicitly.
It also keeps point counts, motion points, UVs, and custom primvars aligned when
splitting a description into at most 65,536-strand batches.

```cpp
#include <nanoxgen/curve_payload.h>

using namespace nanoxgen;

GeneratedCurves frame = generate_cpu(asset, params);
GeneratedCurves shutter = generate_deformed_cpu(
    asset, params, shutter_geometry);

const MotionSampleView motion{shutter_time, view_generated_curves(shutter)};
const auto batches = build_curve_batches(
    view_generated_curves(frame), std::span{&motion, 1u});
```

When fixed-CV output needs no CPU transform/resampling/primvar merge, use the
direct path instead. It writes the renderer's final `float4` layout in the
generation pass and avoids allocating and rereading a separate position/width
intermediate:

```cpp
PackedGeneratedCurves curves = generate_packed_cpu(asset, params);
```

The optional LuisaCompute backend records typed generation kernels once and
dispatches them through HIP or Vulkan. It uploads validated roots, exact random
identity, CSR guide associations and rebuilt float guides, then writes final
strand-major `float4(position, radius)` buffers. There is no handwritten
CUDA/HIP API and the portable CPU library has no Luisa dependency.

The Classic cold benchmark exercises this complete boundary directly from the
authoring collection and Alembic patch:

```bash
./build/luisa-classic-hip-release/nanoxgen_xgen_classic_luisa_benchmark \
  /external/LuisaCompute-next/build/bin hip \
  /external/rabbit/collection.xgen /external/rabbit/patches.abc \
  /external/rabbit/xgen/collections/collection eyelash \
  --warmup 3 --repeats 11 --reference-nxc /external/oracle/eyelash.nxc
```

The reported cold interval includes file input, native root/guide planning,
device creation, no-cache JIT, upload, execution, download and renderer packing.
Warm dispatch is reported separately and must not be used as a cold-start
speedup.

The verified noise parameters map to Maya as follows:

| NanoXGen field | Maya/XGen value |
|---|---|
| `noise_amplitude` | `magnitude` in scene units |
| `noise_frequency` | `frequency` in cycles per scene-unit of curve length |
| `noise_mask` | scalar `[0, 1]` multiplier |
| `noise_correlation` | UI `correlation / 100` |
| `noise_preserve_length` | UI `preserveLength / 100` |

The current native path implements the default linear magnitude-scale ramp.
Authored ramp curves, expressions, and PTEX/texture masks require additional
asset sections and are not silently approximated.

Motion output remains implemented in the portable CPU payload path and the
evaluated `.nxc` format. A Luisa motion path is not exposed until it can retain
the same exact root identity and pass topology-change/duplicate-identity tests.

## Compiler modes and numerical policy

Release builds use `-O3` by default. `NANOXGEN_NATIVE_ARCH=ON` enables host
ISA tuning, `NANOXGEN_ENABLE_IPO=ON` enables supported interprocedural
optimization. The explicit Luisa benchmark enables its backend fast-math mode
by default and accepts `--strict-math` for differential calibration. Keep the distributable
library portable unless
the deployment CPU baseline is known; native ISA builds should be produced per
render-farm hardware class.

On x86, `NANOXGEN_SIMD_WIDTH=256` (or `SIMD_WIDTH=256` with Make) asks
GCC/Clang to prefer AVX2-width vectorization. The payload loops are confirmed
auto-vectorized, but unrestricted `-march=native` may select AVX-512 and lose
frequency on some CPUs. Benchmark 256 versus 512 per farm hardware class;
wider SIMD is not assumed to be faster.

Fast math is a production throughput mode, never the strict XGen parity mode.
`scripts/run_fast_math_comparison.sh` builds strict and fast binaries, measures
both, compares every generated float, requires identical topology/root triangle
indices, rejects non-finite values, and applies a documented scale-specific
error budget. Any kernel or compiler upgrade must rerun that gate.

Each `CurveBatch` owns its arrays and can be copied or moved into a renderer
curve object. `motion_points` is sample-major; every sample has exactly
`points.size()` absolute positions. `AffineTransform` handles object-to-world
position conversion, while `radius_scale` is separate because a non-uniform
transform has no unique scalar radius transform.

## Animated patches and motion blur

An `.nxg` asset stores rest-pose topology, the area alias table, guides, and
root candidate data. `generate_deformed_cpu` accepts frame-local vertex
positions, normals, and guide CVs without rebuilding that blob. Triangle and
barycentric root samples are derived only from the asset seed and strand ID, so
the same strand identity is preserved across shutter samples. This mirrors the
renderer behavior of evaluating an IGS description once for the base frame and
again for motion.

The alias table intentionally remains rest-area weighted under deformation.
Rebuilding it per frame would change discrete triangle choices and cause motion
correspondence to break.

For static or baked descriptions, `nanoxgen_xgen_cache` independently converts
one evaluated XGen BLOB plus optional separately evaluated shutter samples into a pointer-free
`.nxc` curve cache. Curves are canonically matched by
`faceId + faceUV + patchUV` before motion is stored; topology changes or
duplicate identities fail instead of silently pairing the wrong curves. The
renderer-minimal mode retains point counts and bit-exact
`float4(position, radius)` plus aligned motion positions. The full mode also
retains public XGen texcoords, patch UVs, face UVs, and face IDs.

```bash
nanoxgen_xgen_cache --renderer-minimal \
  --motion 0.5 shutter-close.xgen base.xgen groom.nxc
```

This cache path reproduces evaluated official geometry; it is not a claim that
NanoXGen procedurally reproduced the underlying XGen modifiers. Its purpose is
to remove Maya/XGen from BLOB parsing and repeated static renders while
procedural parity is expanded. `nanoxgen_xgen_process` can also transform and
rewrite an evaluated BLOB while preserving metadata, reference meshes, unknown
arrays, and group layout. An explicit `--rebuild` emits a minimal evaluated
BLOB from the renderer-relevant curve view.

## Coverage relative to the existing XGen paths

| Renderer requirement | NanoXGen status |
|---|---|
| Positions and per-CV radius | implemented on CPU and in the Luisa Classic kernels |
| Fixed or variable point counts in output | implemented; Luisa currently compacts Cut-culled fixed-CV strands after download |
| Root UV, uniform float, uniform color | implemented payload contract |
| Multiple absolute motion samples | implemented payload contract and evaluated `.nxc`; Luisa generation pending |
| Deformed mesh/normal/guide overlays | implemented on CPU; Luisa Classic currently evaluates one imported sample |
| Object-to-world transform | implemented |
| 65,536-strand chunking | implemented and boundary-tested |
| Optional uniform CV resampling | implemented |
| Frame-stable deterministic roots | implemented for NanoXGen assets and Classic RandomGenerator |
| Exact XGen root RNG | implemented for the supported Classic random-generator path |
| Cut and width taper parity | CPU/Luisa implemented; exact Rabbit eyelash topology verified |
| XGen noise core | CPU/Luisa implemented and oracle-verified for supported mode-0 bindings |
| XGen clump/coil/collision | not implemented |
| XGen expressions and PTEX | float runtime plan wires length/width/taper/ramp/reparameterized Cut to CPU and Luisa lowering; PTEX and remaining FX passes remain |
| Camera-frustum generation culling | not implemented |
| Classic archive/card/sphere primitives | outside the current curve-only scope |

The payload layer makes NanoXGen usable without Autodesk libraries once curve
generation is complete. It does not by itself reproduce the authoring behavior
of every XGen module. Algorithm parity and renderer transport are measured and
reported separately.

## Performance accounting

Benchmarks use four distinct stages:

- `native_ms`: complete NanoXGen root sampling, guide interpolation, modifiers,
  and generated-array allocation;
- `renderer_pack_ms`: conversion to point counts, `float4(position, radius)`,
  UVs, motion, primvars, transforms, and batches;
- `linear_modifier_reference_ms`: only linear CV reconstruction and verified
  cut/taper arithmetic from official root/tip seeds;
- XGen `load`, `execute`, and `materialize`: BLOB parsing, render-time script
  execution, and copying public iterator data into renderer-owned arrays.

The linear modifier reference is deliberately not called a generation path and
must not be used to claim end-to-end speedup.
