# Renderer integration contract

This document maps NanoXGen to the curve payload consumed by a production Maya
renderer integration. It covers both classic `xgmDescription` data received
through `XGenRenderAPI::PrimitiveCache` and Interactive Grooming data received
through `XGenSplineAPI::XgFnSpline`.

These Maya/XGen APIs describe calibration and one-time interchange paths, not
the NanoXGen runtime dependency graph. The core renderer path consumes `.nxg`
or `.nxc` and does not link Autodesk libraries. Raw Interactive Grooming
`outRenderData` `.xgen` BLOBs still require the optional calibration converter
until an independent decoder is implemented.

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

The checked `launch_generate_packed_cuda` overload provides the same contract
on a CUDA stream and writes directly into renderer-owned device memory. The
descriptor carries capacities because a raw device pointer cannot be inspected
safely on the host. It rejects undersized buffers, mismatched deformation
arrays, invalid numeric parameters, and invalid launch geometry before the
kernel is enqueued. The packed kernel writes fixed `pointCounts`, root UVs, and
`float4(position, radius)` in the same strand pass.

```cpp
// d_asset already contains an exact copy of asset.bytes().
DeviceAssetDescriptor gpu_asset = make_device_asset_descriptor(
    asset, d_asset, d_asset_capacity);
DevicePackedCurveOutputDescriptor output{
    {d_points, d_roots, d_root_uvs, radius_scale, d_point_counts},
    point_capacity, root_capacity, root_uv_capacity, point_count_capacity};

cudaError_t error = launch_generate_packed_cuda(
    gpu_asset, {}, params, output, {}, stream);
```

The original raw-pointer overload remains available for tightly controlled
internal code, but it cannot prove allocation sizes and should not be used at
the renderer boundary.

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

`launch_generate_motion_cuda` accepts all shutter overlays together. It first
validates every deformation descriptor, strictly increasing finite sample
times, and the complete sample-major output capacity. Only after the whole
request is valid does it enqueue one position-only kernel per sample on the
same stream. Every launch reuses the base asset, seed, strand count, and CV
count, preserving root identity while avoiding widths/UVs in motion buffers.

### CUDA validation and benchmarking

Configure with `-DNANOXGEN_ENABLE_CUDA=ON`. When tests are enabled, the build
also creates `nanoxgen_cuda_tests`. It compares the shared CPU and CUDA math for
XGen-compatible noise, 40% length preservation, packed renderer output,
deformed geometry, and sample-major motion. The test returns CTest's explicit
skip code when no CUDA device is visible; a successful NVCC build alone is not
reported as device validation.

For a real `.nxg` asset, benchmark direct renderer-buffer generation with:

```bash
./build/cuda-release/nanoxgen_cuda_benchmark groom.nxg --strands 100000 --cvs 12
./build/cuda-release/nanoxgen_cuda_benchmark groom.nxg --strands 100000 --cvs 12 --noise
```

The JSON result reports the CUDA device, compute capability, median/p95 kernel
time, and generated-CV throughput. Transfers and renderer submission are
intentionally outside the timed region; the asset and output allocations stay
GPU-resident across repetitions.

## Compiler modes and numerical policy

Release builds use `-O3` by default. `NANOXGEN_NATIVE_ARCH=ON` enables host
ISA tuning, `NANOXGEN_ENABLE_IPO=ON` enables supported interprocedural
optimization, and `NANOXGEN_FAST_MATH=ON` enables relaxed floating-point math
(`--use_fast_math` for CUDA). Keep the distributable library portable unless
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

For static or baked descriptions, `nanoxgen_xgen_cache` converts one official
XGen BLOB plus optional separately evaluated shutter samples into a pointer-free
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
to remove Maya/XGen and `MPxData::writeBinary` from repeated static renders
while procedural parity is expanded.

## Coverage relative to the existing XGen paths

| Renderer requirement | NanoXGen status |
|---|---|
| Positions and per-CV radius | implemented, including checked CUDA direct output |
| Fixed or variable point counts in output | implemented; fixed counts are fused into CUDA generation |
| Root UV, uniform float, uniform color | implemented payload contract |
| Multiple absolute motion samples | implemented payload contract and checked CUDA sample-major output |
| Deformed mesh/normal/guide overlays | implemented on CPU and shared device math |
| Object-to-world transform | implemented |
| 65,536-strand chunking | implemented and boundary-tested |
| Optional uniform CV resampling | implemented |
| Frame-stable deterministic roots | implemented with NanoXGen RNG |
| Exact XGen root RNG | not implemented; use explicit XGen seeds for parity work |
| Cut and width taper parity | verified on linear official fixtures |
| XGen noise core | implemented and oracle-verified; scalar mask/default linear magnitude ramp only |
| XGen clump/coil/collision | not implemented |
| XGen expressions and PTEX | not implemented |
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
