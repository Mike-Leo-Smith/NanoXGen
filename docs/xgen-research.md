# XGen research notes and NanoXGen mapping

## Verified SDK boundary

XGen is not distributed as a standalone open-source SDK. The public Maya DevKit
contains `FindXGen.cmake`, but that module searches a full Maya install at
`<Maya>/plug-ins/xgen` for headers and `libAdskXGen.so`. This was verified
against the real Maya 2027.1 Linux runtime. In that release the spline header is
`include/xgen/src/xgsculptcore/api/XgSplineAPI.h`. NanoXGen therefore treats
Autodesk XGen as an optional import and differential-testing bridge. The core
format and kernels do not link against Autodesk code.

## Classic XGen model

The original XGen design composes four independent modules into a Description:

1. patch: binds a procedural to mesh faces and exposes face IDs, area, position,
   normal, and reference-pose data;
2. generator: emits primitive roots randomly, uniformly, or from stored points;
3. primitive: splines, cards, spheres, or archive instances;
4. renderer/output: preview, renderer integration, or caches such as XPD/XUV.

Collections contain Descriptions. `.xgen`/`.xdsc` store configuration, `.xuv`
stores generated root locations, `.xgc` stores guide connectivity, and `.xpd`
stores baked primitive geometry and attributes per patch. Interactive Grooming
uses a different Maya-node pipeline and exposes a serialized `outRenderData`
BLOB through `XgFnSpline`/`XgItSpline`.

## Documented guide interpolation

Classic XGen constructs approximate Voronoi neighborhoods for guides by adaptive
subdivision on patch faces. Each guide gets a direction-dependent range of
influence represented by sweep angles and radii. For a generated primitive:

1. reject unrelated guides using neighborhood, surface curvature, and region
   information;
2. keep guides whose range of influence contains the root;
3. weight a guide by the distance from the root to that range boundary;
4. normalize weights;
5. rebuild active guides to the same CV count, translate them to the primitive
   root, and compute a weighted average per CV ("relative interpolation").

## NanoXGen v0.1 mapping

| XGen concept | NanoXGen v0.1 | Status |
|---|---|---|
| Patch/PRef | indexed triangle mesh and rest-area alias table | implemented |
| Random generator | counter-based deterministic sampling + area alias table | implemented |
| Active guide pruning | fixed eight-guide stencil per triangle | implemented approximation |
| Range of influence | isotropic compact support radius | implemented approximation |
| Relative interpolation | translate guide to sampled root, resample and blend CVs | implemented |
| Width/taper | linear root-to-tip width | implemented |
| Noise modifier | strand-stable sinusoidal displacement | initial implementation |
| XGen expressions/PTEX | compiled expression IR and texture sampling | planned |
| Geodesic guide neighborhoods | surface graph / UV-space acceleration | planned |
| Clump/collision/coil/wind | ordered GPU modifier graph | planned |
| Interactive Groom BLOB import | canonical, bit-exact `.nxc` evaluated cache | validated with Maya 2027.1 |

The v0.1 guide stencil deliberately moves expensive guide selection to asset
construction. Runtime work is bounded by `strand_count * cvs_per_strand * 8`,
with one GPU thread per strand. CUDA keeps a direct static mapping; on CPU,
persistent worker threads dynamically claim CUDA-block-sized strand tiles
through an atomic counter.

## Maya 2027.1 real-fixture results

The harness creates a two-by-two-unit polygon plane and calls Maya's real
Interactive Grooming generator. It exports `outRenderData` with
`xgmExportSplineDataInternal`, then parses the result through the public
`XgFnSpline` API rather than through a NanoXGen approximation.

| Fixture | Density | Length | Width | CVs/curve | Curves | Vertices | Canonical hash |
|---|---:|---:|---:|---:|---:|---:|---|
| baseline A | 4 | 1.0 | 0.02 | 5 | 16 | 80 | `0x6ec28960f29c73fd` |
| baseline B | 4 | 1.0 | 0.02 | 5 | 16 | 80 | `0x6ec28960f29c73fd` |
| variant | 2 | 0.5 | 0.03 | 7 | 8 | 56 | `0x7bb6a42e72397617` |

The two baseline exports had different raw file hashes but identical canonical
geometry. Therefore the serialized container contains nondeterministic bytes,
while the tested positions, widths, curve texture coordinates, and patch UVs
were deterministic. The variant also confirmed that density, length, width, and
CV-count changes propagate through the real runtime as expected. An empty BLOB
was rejected by `XgFnSpline` with an invalid-stream error.

Canonical hashing sorts curves by face ID, face-local UV, and patch UV before
hashing. This is necessary because modifier evaluation may change BLOB curve
order without changing the set of curves.

## Complex strict-parity fixture

The complex source is a 4-by-3 plane subdivided 8-by-6 and displaced by a
deterministic analytic wave. At density 20 it produces 244 strands with 9 CVs
each. Four independent official fixtures cover base generation, a repeated
base, 27% cut plus 0.8 width taper starting at 0.25, and a live noise-plus-cut
chain.

The strict compatibility boundary is intentionally explicit. This is called
the **linear modifier reference**, not a strict/full generation path. The public XGen
base output is reduced to one `LinearCurveSeed` per strand: root, unmodified
tip, patch UV, and root width. NanoXGen regenerates all CVs and applies cut and
taper. This validates curve generation and modifier arithmetic independently of
XGen's undocumented root RNG. It does **not** claim that NanoXGen's native root
sampler already reproduces XGen's root distribution.

| Check | Result |
|---|---:|
| Curves / CVs | 244 / 2,196 |
| Face ID, face UV, patch UV mismatches | 0 |
| Width values bitwise identical | 2,196 / 2,196 |
| Position components | 6,588 |
| Position components bitwise identical | 4,705 / 6,588 |
| Position max absolute error | `2.38418579e-7` |
| Position RMS error | `3.52377918e-8` |
| Position components outside `(5e-7 abs OR 4 ULP)` | 0 |

The non-bitwise position values are caused by Maya/XGen's higher-precision
intermediate evaluation before float output. Near zero, ULP counts can be large
despite sub-micro-unit absolute error, so the regression requires either the
absolute or ULP bound while still reporting both. Width evaluation is bitwise
identical. Noise remains an oracle-only fixture because NanoXGen's current
sinusoidal noise is not XGen's random field; clump and guide modifiers remain
future parity work.

## CPU and renderer-payload performance snapshot

Measured in one shared container with 9 logical CPUs, optimized builds, 12 CVs
per strand, and median times. Host scheduling and frequency introduce run-to-run
variation, so these numbers describe this machine rather than a universal
speedup.

| Curves | NanoXGen direct renderer output | NanoXGen generation + generic pack | XGen already-BLOB load + execute + materialize | Minimal `.nxc` read + full validation | Maya invalidated export |
|---:|---:|---:|---:|---:|---:|
| 10,000 / 9,970 XGen | 0.773 ms | 0.898 ms | 10.940 ms | 0.789 ms | 355.908 ms |
| 100,000 / 99,929 XGen | 6.044 ms | 8.202 ms | 29.889 ms | 8.1–9.5 ms | 558–562 ms |

Direct renderer output writes `float4(position, radius)` during generation and
avoids the separate position/width intermediate. The generic pack remains for
variable CV counts, transforms, resampling, primvar merging, and owning 64K
batches. XGen already-BLOB ingestion is still not an identical algorithm
comparison: it parses a proprietary, more general result while NanoXGen
implements fewer authoring features.

The linear modifier reference is intentionally excluded from speedup claims: it
starts from official root/tip seeds and performs neither root sampling nor guide
interpolation. The Maya export measurement includes DG host overhead, BLOB
construction, render-time processing, and serialization, so it is also reported
separately. NanoXGen generation and packing include output allocations but no
disk write.

## Renderer bridge profiling and exact-cache fallback

The actual IGS renderer call sequence was reproduced inside Maya:

`MPlug::asMObject -> MPxData::writeBinary -> XgFnSpline::load -> executeScript -> materialize`.

At roughly 100,000 curves / 1.2 million CVs, median/range measurements were:

| Stage | Time |
|---|---:|
| `asMObject` / DG evaluation | about 0.015 ms |
| `MPxData::writeBinary` | 548.6–564.8 ms |
| redundant `stringstream::str()` copy | about 0.317 ms |
| `XgFnSpline::load` | 16.4–20.5 ms |
| `executeScript` | 0.60–0.69 ms |
| renderer materialization | 7.1–10.6 ms |

Changing `stringstream` to a reserved custom stream did not materially improve
the result. The public spline API accepts a BLOB produced through
`MPxData::writeBinary`; neither the public header nor Autodesk's SDK example
provides a typed direct accessor for `outRenderData`. Therefore container tuning
cannot remove the dominant serialization cost.

For unchanged assets, `nanoxgen_xgen_cache` pays the official evaluation once
and writes `.nxc`. A 99,929-curve renderer-minimal cache was 19.6 MB, preserved
all public position/radius floats bit-for-bit, and read plus fully validated in
8.1–9.5 ms. The full 31.2 MB version also preserves texcoords, patch UVs, face
UVs, and face IDs and validated in about 14.2 ms. Runtime loading has no Autodesk
dependency. This is exact evaluated-geometry parity, not procedural algorithm
parity.

Repeated official evaluations were also observed to return identical canonical
geometry in different raw curve orders. Motion samples are therefore matched by
`faceId + faceUV + patchUV`; topology changes and duplicate/ambiguous identities
are rejected. Appending a second evaluation purely by iterator order is unsafe.

## SIMD and fast-math findings

GCC's vectorization report confirms the contiguous renderer-pack loops use
vector instructions (up to 64-byte vectors on the test CPU). Native AVX-512 was
not consistently faster because wide-vector frequency effects compete with the
memory-bound pack. A 256-bit preference is exposed for per-hardware tuning.

The precision fixture uses a wave surface, 16 nonuniform guides, noninteger
weight power, and live noise. Across 31 repeats, fast math reduced the median
from 5.572 to 5.262 ms (5.6%) on this CPU. It kept root triangle indices exact,
introduced no non-finite values, and produced:

| Array | Maximum absolute error | RMS error |
|---|---:|---:|
| positions | `1.43051147e-6` | `1.87759391e-7` |
| widths | `1.86264515e-9` | `9.18501397e-10` |
| root float attributes | `9.53674316e-7` | `1.03593679e-7` |

This passed the fixture's relaxed, scene-scale-specific production budget but
did not pass strict XGen parity: 8,193 of 960,000 position components were
outside `(5e-7 absolute OR 4 ULP)`. Fast math remains opt-in and must be tested
separately on the target CUDA architecture before enabling `--use_fast_math`.

One benchmark pitfall was found while adding renderer materialization:
`XgItSpline::vertexCount()` must be cached once per iterator batch. Calling it
inside the per-primitive loop inflated a 1.2-million-CV copy from milliseconds
to seconds. The benchmark now separates iterator metadata access from the
actual renderer-buffer copy.

## Next implementation phases

1. Integrate direct packed generation and `.nxc` fallback into the renderer,
   including cache invalidation and canonical shutter-sample matching.
2. Expand licensed fixtures to capture authored guide inputs and individual
   modifier outputs, not only evaluated final curves.
3. Replace the isotropic support kernel with UV/geodesic, direction-dependent
   guide regions and validate interpolation error against those fixtures.
4. Compile a restricted XGen-expression subset to a typed GPU bytecode/IR and
   add PTEX sampling through a texture indirection table.
5. Implement modifier passes in dependency order: cut/length, clump, noise,
   coil, collision, and wind. Fuse passes where locality permits and use
   explicit intermediate curve buffers where modifiers need neighborhoods.
6. Compile and benchmark the existing direct CUDA kernel on target renderer
   GPUs, add a LuisaCompute backend, and compare throughput, memory,
   determinism, and strict/fast-math error against the XGen CPU baseline.

## Clean-room rule

Algorithm work should use Autodesk's public documentation, public SDK surface,
and controlled input/output comparisons. Do not copy proprietary implementation
code or depend on undocumented in-memory layouts. Differential fixtures should
store our own inputs and numeric outputs, not Autodesk binaries.

## Public references

- Autodesk Maya API/DevKit downloads:
  <https://aps.autodesk.com/developer/overview/maya-api>
- Autodesk's XGen Interactive Grooming SDK example and `XgFnSpline` layout:
  <https://blog.autodesk.io/support-xgen-interactive-grooming-feature-in-your-plugin/>
- Autodesk's documented guide neighborhood and interpolation process:
  <https://download.autodesk.com/global/docs/maya2014/en_us/files/GUID-49B8C299-71DD-4341-8638-97437D9A328F.htm>
- Autodesk's XGen file inventory:
  <https://download.autodesk.com/global/docs/maya2014/en_US/files/GUID-66DECD58-3AEC-46B1-8CC7-FD968B60B044.htm>
- Thompson, Petti, and Tappan, *XGen: Arbitrary Primitive Generator*:
  <https://forums.autodesk.com/autodesk/attachments/autodesk/maya-ideas-en/2810/2/XGen%20Arbitrary%20Primitive%20Generator.pdf>
- MIT-licensed `XgFnSpline` to CyHair reference converter:
  <https://github.com/syoyo/xgen-spline-to-cyhair>
