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

The checked `launch_generate_packed_cuda` and `launch_generate_packed_hip`
overloads provide the same contract on CUDA and HIP streams and write directly
into renderer-owned device memory. The
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

The AMD path uses the same descriptors and replaces only the backend entry
point and stream/error types:

```cpp
#include <nanoxgen/hip.h>

hipError_t error = launch_generate_packed_hip(
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

`launch_generate_motion_cuda` and `launch_generate_motion_hip` accept all
shutter overlays together. They first
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

### AMD HIP validation and benchmarking

Configure the `hip-release` preset with a ROCm HIP compiler and the target AMD
architecture when CMake cannot infer them. The build creates
`nanoxgen_hip_tests`, `nanoxgen_hip_benchmark`, and
`nanoxgen_hip_cache_benchmark`. The parity test exercises packed output,
deformed geometry, noise/length preservation, and sample-major motion against
the CPU implementation on a real device; compilation alone is not reported as
runtime validation.

```bash
cmake --preset hip-release \
  -DCMAKE_HIP_COMPILER=/opt/rocm/lib/llvm/bin/clang++ \
  -DCMAKE_HIP_ARCHITECTURES=gfx1201
cmake --build --preset hip-release
ctest --preset hip-release
./build/hip-release/nanoxgen_hip_benchmark groom.nxg \
  --strands 100000 --cvs 12 --noise
```

For already evaluated `.nxc` assets, the residency benchmark keeps performance
stages explicit:

```bash
./build/hip-release/nanoxgen_hip_cache_benchmark \
  --warmup 3 --repeats 15 /external/rabbit-cache/*.nxc
```

It loads and validates every cache on the CPU, concatenates only point counts
and renderer `float4` points, uploads them, and runs a GPU kernel over every
element. The kernel rejects non-finite positions/radii and negative radii,
sums topology, and computes a deterministic bitwise checksum which must match
the host. This is a renderer-residency/memory-validation measurement, not a
claim that HIP evaluates an Autodesk authoring graph.

## Compiler modes and numerical policy

Release builds use `-O3` by default. `NANOXGEN_NATIVE_ARCH=ON` enables host
ISA tuning, `NANOXGEN_ENABLE_IPO=ON` enables supported interprocedural
optimization, and `NANOXGEN_FAST_MATH=ON` enables relaxed floating-point math
(`--use_fast_math` for CUDA and `-ffast-math` for HIP). Keep the distributable
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
| Positions and per-CV radius | implemented, including checked CUDA/HIP direct output |
| Fixed or variable point counts in output | implemented; fixed counts are fused into GPU generation |
| Root UV, uniform float, uniform color | implemented payload contract |
| Multiple absolute motion samples | implemented payload contract and checked GPU sample-major output |
| Deformed mesh/normal/guide overlays | implemented on CPU and shared device math |
| Object-to-world transform | implemented |
| 65,536-strand chunking | implemented and boundary-tested |
| Optional uniform CV resampling | implemented |
| Frame-stable deterministic roots | implemented with NanoXGen RNG |
| Exact XGen root RNG | not implemented; use explicit XGen seeds for parity work |
| Cut and width taper parity | verified on linear official fixtures |
| XGen noise core | implemented and oracle-verified; scalar mask/default linear magnitude ramp only |
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
