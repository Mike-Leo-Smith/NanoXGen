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
dispatches them through HIP, Vulkan, or Luisa's CPU fallback backend. It
uploads validated roots, exact random identity, CSR guide associations and
rebuilt float guides, then writes final strand-major
`float4(position, radius)` buffers. There is no handwritten CUDA/HIP API and
the portable CPU library has no Luisa dependency.

The renderer owns Luisa's `Context`, `Device`, `Stream`, and every buffer. It
passes its existing `Device` directly to NanoXGen; the renderer-facing API does
not take a backend name and never creates a second device:

```cpp
#include <nanoxgen/luisa/xgen_classic_collection.h>
#include <nanoxgen/xgen_classic_collection.h>

using namespace nanoxgen::luisa_backend;

// Keep one CPU execution context across host preparation and JIT. The default
// owns an affinity-aware pool; NanoXGenContext{renderer_executor} borrows an
// existing renderer scheduler instead.
nanoxgen::NanoXGenContext nanoxgen_context;

nanoxgen::ClassicCollectionExecutionOptions host_options{};
host_options.context = &nanoxgen_context;
auto plan = nanoxgen::build_xgen_classic_collection_execution_plan(
    collection, collection_path, archive_path, descriptions_root,
    host_options);

std::vector<ClassicCollectionCompileInput> inputs = make_compile_inputs(plan);
ClassicCollectionCompileOptions options{};
options.context = &nanoxgen_context;

ClassicCollectionPipeline pipeline =
    compile_classic_collection(renderer_device, inputs, options);

ClassicCollectionDispatchResources resources =
    make_views_of_renderer_owned_buffers(description);
pipeline.encode(
    renderer_stream, description_index, resources, strand_count);
// The renderer decides when to submit, synchronize, or read the buffers.
```

`make_compile_inputs` and `make_views_of_renderer_owned_buffers` above are
renderer-adapter placeholders, not NanoXGen API functions. They populate the
public `ClassicCollectionCompileInput` and
`ClassicCollectionDispatchResources` structs declared by the included header;
the benchmark tool is the complete allocation/upload example.

`compile_classic_collection` specializes all descriptions as one batch and
owns the resulting shaders. `encode` only records commands: it performs no
allocation, upload, synchronization, or download. Buffer views and the Device
must outlive their recorded work, and the Device must outlive the pipeline.
`ClassicCollectionExecutionPlan` is the backend-neutral host representation
for one master Classic `.xgen`; it carries all descriptions rather than asking
the renderer to reopen the main file per description.
Descriptions, PTEX sampling, clump binding, guide-runtime evaluation, and JIT
all submit work to the context's single dynamic queue. There is no fixed
description-by-inner-worker product and no nested thread creation. Work size
controls how many tasks are exposed, while physical concurrency never exceeds
the context executor's worker count. If no context is supplied, each top-level
API creates an affinity-aware context for that call and releases it on return.
Large LLVM compile batches use an adaptive lane limit of roughly 75% for
contexts above four workers; this avoids cold-JIT allocator/memory-bandwidth
contention while host preparation can still consume the full context.

The Classic cold benchmark exercises this complete boundary directly from the
authoring collection and Alembic patch. Omitting DESCRIPTION selects the
collection-wide, one-Device path:

```bash
./build/luisa-classic-hip-release/nanoxgen_xgen_classic_luisa_benchmark \
  /external/LuisaCompute-next/build/bin hip \
  /external/rabbit/collection.xgen /external/rabbit/patches.abc \
  /external/rabbit/xgen/collections/collection \
  --threads 0 --warmup 0 --repeats 1
```

The reported cold interval includes file input, native root/guide planning,
device creation, no-cache JIT, upload, execution, download and renderer packing.
Warm dispatch is reported separately and must not be used as a cold-start
speedup.

### Classic motion samples

Classic motion uses the same external `Device`, `Stream`, and
`NanoXGenContext`. Lookup values are frame offsets used to evaluate Alembic at
`(frame + lookup) / fps`; placements are the renderer's interpolation times
and do not select archive samples. `shutterOffset` remains renderer metadata,
not another geometry time.

```cpp
nanoxgen::ClassicMotionSampling sampling{};
sampling.frame = current_frame;
sampling.frames_per_second = fps;
sampling.lookup_offsets.assign(lookup.begin(), lookup.end());
sampling.placements.assign(placement.begin(), placement.end());

auto motion_plan =
    nanoxgen::build_xgen_classic_collection_motion_execution_plan(
        collection, collection_path, archive_path, project_or_data_root,
        sampling, host_options);

ClassicCollectionPipeline pipeline =
    compile_classic_collection(renderer_device,
                               make_compile_inputs(motion_plan),
                               compile_options);

// One entry per placement. Invariant CSR, runtime-input and clump-index
// buffers can alias; roots, guides, clump axes, states and final points are
// per unique deformation.
pipeline.encode_motion(
    renderer_stream, description_index, sample_resources,
    sampling.placements, deformation_sources, strand_count);
```

The host plan accepts 1--20 samples, rejects non-finite or non-increasing
tables, preserves the authored root identity, and rejects duplicate
`(patch, faceId, bit(u), bit(v))` identities or changing Alembic topology.
It builds the root-to-coarse-face index once, so later samples are linear in
the number of roots. Static archives, repeated lookups, and bit-identical
deformations expose `deformation_source_index`; the renderer should bind the
source sample's final point buffer instead of dispatching or copying it again.
Transform stacks are interpolated by Alembic operation channel rather than by
linearly blending a 4x4 matrix.

The supplied `CurveParser.cpp` currently consumes two samples: base
`float4(position, radius)` in `Curve::points` and the second sample's absolute
xyz in `Curve::pointMotions`. Bind motion-plan sample zero to `points`, resolve
sample one through its `deformation_source_index`, and copy/view that final
buffer as `pointMotions`. Apply scene offsets and the object transform to both
samples. More than two samples require extending that renderer container, but
NanoXGen does not impose the two-sample restriction.

Do not construct `${DESC}` paths as `supplied_root / description` directly.
Call `resolve_xgen_classic_descriptions_root`, or use either collection-plan
builder, which calls it automatically. The supplied argument may be the final
collection data directory or a project root; `xgDataPath`, relocated
`xgProjectPath`, `${PROJECT}xgen/...`, and mixed `\`/`/` separators are
handled without a recursive filesystem scan. Plain relative `xgDataPath`
values are anchored to the supplied project; ambiguous drive-relative
(`C:xgen/...`) and root-relative (`\xgen/...`) Windows values are rejected.
Description-side PTEX/XUV paths additionally accept either separator, plain
relative paths, and stale absolute paths whose suffix can be proven to exist
under the resolved description directory. Prefer `${DESC}` in newly authored
packages because it is unambiguous across drive letters, mount points, and
platforms.

The verified noise parameters map to Maya as follows:

| NanoXGen field | Maya/XGen value |
|---|---|
| `noise_amplitude` | `magnitude` in scene units |
| `noise_frequency` | `frequency` in cycles per scene-unit of curve length |
| `noise_mask` | scalar `[0, 1]` multiplier |
| `noise_correlation` | UI `correlation / 100` |
| `noise_preserve_length` | UI `preserveLength / 100` |

The compact `.nxg` prototype path implements only its default linear
magnitude-scale ramp. The separate Classic collection pipeline implements the
calibrated `rampUI` modes and root-sampled scalar PTEX expressions described in
`classic-native.md`. General vector expressions and texture/modifier bindings
outside that bounded Classic plan remain checked fallbacks rather than silent
approximations.

Motion output is implemented in the portable payload path, evaluated `.nxc`,
Classic Alembic host planner, and Luisa collection pipeline. The typed Autodesk
bridge remains the oracle for `PrimitiveCache` sample topology and absolute
positions.

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
| Multiple absolute motion samples | implemented for payload/`.nxc` and 1--20 Classic Luisa samples with deformation aliases |
| Deformed mesh/normal/guide overlays | implemented on CPU and per unique Classic Luisa motion deformation |
| Object-to-world transform | implemented |
| 65,536-strand chunking | implemented and boundary-tested |
| Optional uniform CV resampling | implemented |
| Frame-stable deterministic roots | implemented for NanoXGen assets and Classic RandomGenerator |
| Exact XGen root RNG | implemented for the supported Classic random-generator path |
| Cut and width taper parity | CPU/Luisa implemented; exact Rabbit eyelash topology verified |
| XGen noise core | CPU/Luisa implemented and oracle-verified for supported mode-0 bindings |
| XGen clump/coil/collision | hierarchical Rabbit ClumpingFX implemented; coil and collision remain fallbacks |
| XGen expressions and PTEX | float runtime plan and CPU/Luisa lowering include root-sampled PTEX `map()`, palette scalars, and `noise($Prefg*constant)`; general vector SeExpr and remaining FX passes remain |
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
