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

## NanoXGen v0.2 mapping

| XGen concept | NanoXGen v0.2 | Status |
|---|---|---|
| Patch/PRef | indexed triangle mesh and rest-area alias table | implemented |
| Random generator | counter-based deterministic sampling + area alias table | implemented |
| Active guide pruning | fixed eight-guide stencil per triangle | implemented approximation |
| Range of influence | isotropic compact support radius | implemented approximation |
| Relative interpolation | translate guide to sampled root, resample and blend CVs | implemented |
| Width/taper | linear root-to-tip width | implemented |
| Noise modifier | 3D gradient field, transported frame, correlation and length preservation | scalar/length model oracle-verified |
| XGen expressions/PTEX | bounded scalar IR plus root-sampled PTEX input tables | implemented for the calibrated Classic bindings; general vector SeExpr remains |
| Geodesic guide neighborhoods | surface graph / UV-space acceleration | planned |
| Clump/collision/coil/wind | hierarchical clump CPU/Luisa path; remaining modifiers | partial |
| Interactive Groom BLOB import/export | independent v1 parser/writer plus canonical `.nxc` cache | bit-exact oracle-validated with Maya 2027.1 |

The guide stencil deliberately moves expensive guide selection to asset
construction. Runtime work is bounded by `strand_count * cvs_per_strand * 8`,
with one Luisa work item per strand. On CPU, workers in the caller's
`NanoXGenContext` or a scoped internal pool dynamically claim fixed-size strand
tiles through an atomic counter; NanoXGen does not require a process-lifetime
global pool.

## Maya 2027.1 real-fixture results

The harness creates a two-by-two-unit polygon plane and calls Maya's real
Interactive Grooming generator. It exports `outRenderData` with
`xgmExportSplineDataInternal`, then parses each result both through NanoXGen's
standalone v1 decoder and the public `XgFnSpline` oracle.

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
identical. Noise arithmetic is verified separately against official per-curve
fields, as described below. The later Rabbit calibration additionally covers
XPD/PTEX-bound hierarchical clumping on CPU.

## Classic RandomGenerator sample sequence

Maya 2027 exposes `getSample`, which revealed sixteen fixed 32768-point double
tables plus a deterministic tiled extension and eight description-ID square
symmetries. NanoXGen embeds those exact double bit patterns and narrows only
the final root ABI to float. On Rabbit this reproduces per-face candidate and
accepted root counts for all nine descriptions; PTEX density rejection remains
a host planning step.

This data is not PBRT-v4's PMJ02-BN table. Against PBRT-v4 revision
`5f7a606806a4ac7b939131ded9d7a30ebd02416e`, PBRT contains five 65536-point
`uint32` sets, while XGen contains sixteen 32768-point double sets. Quantizing
all 524288 XGen pairs to PBRT's 32-bit representation produced zero exact pair
matches, including swapped coordinates and all eight aligned square
symmetries. The sequences can share low-discrepancy/blue-noise goals without
being the same resident table.

## Modifier-identification harness

`scripts/run_xgen_modifier_study.sh` isolates official IGS modifier behavior and
dumps the public Maya attribute schemas for noise, cut, clump, coil, and
collision candidate nodes. This prevents implementation from being built around
guessed node or attribute names. Maya 2027.1 does not instantiate
`xgmModifierCoil` as a known node, so the harness records it as unavailable
instead of treating the resulting unknown-node placeholder as a valid schema.

The initial matrix varies noise magnitude, frequency, correlation, and
`preserveLength`, repeats an identical fixture, and evaluates both
`noise -> cut` and `cut -> noise`. The companion differential probe canonically
matches curves by `faceId + faceUV + patchUV` and reports:

- root drift and deterministic displacement hashes;
- per-CV displacement envelopes;
- tangent and normal displacement components;
- base/target arc-length ratios and relative errors;
- width changes;
- distance-binned cosine correlation between tip displacement vectors.

The first verifier asserts model-independent invariants: zero magnitude is an
identity, roots remain fixed, identical fixtures repeat, magnitude scales
displacement monotonically, parameters change the field, full length
preservation does not worsen arc-length error, and modifier order is observable.
The second verifier performs a direct numerical reconstruction. The identified
core behavior is:

- a quintic-smoothed 3D gradient field using the 256-vector SeExpr table;
- distance along the control polygon samples the field independently on its
  three axes;
- effective frequency is `max(0.5 / curveLength, frequency)`;
- the UI correlation percentage maps to root-domain scale
  `100 * (1 - correlation / 100)^2`;
- the default magnitude scale is a linear root-to-tip ramp;
- the local vector includes a tangential component and is transformed through
  a parallel-transported surface frame;
- `preserveLength` uniformly scales the noisy control polygon around its root
  to the blend of original and noisy lengths.

The high-CV flat fixtures recover only each official curve's surface-frame
rotation from the fully correlated result. They then predict all other fields
without fitting noise values, amplitude, frequency, or correlation. Across
correlation values 0, 25, 50, 75, 90, 95, 99, and 100, the worst component RMS
was `1.24e-7` and the maximum absolute error was `1.91e-6`. Uniform scaling
predicted the official 100% and 40% length-preservation fields with RMS
`8.42e-8` and `7.62e-8`, respectively. Separate fixtures verify magnitude
linearity and the `0.5 / curveLength` frequency floor.

The shipped Autodesk kernel was used as an interoperability reference, but its
source and private headers are not redistributed or copied into NanoXGen. The
gradient table is the separately licensed Disney SeExpr table; its complete
BSD-3-Clause notice is retained in `LICENSES/SeExpr-BSD-3-Clause.txt`. The
CPU implementation and separately recorded Luisa kernels are independently
guarded by fixed scalar samples, motion-stability tests, and the official output
matrix.

Current procedural boundaries remain explicit: the bounded Classic IR supports
the scalar expressions, PTEX/table bindings, ramps, ClumpingFX, NoiseFX,
CutFX, and width operations calibrated by the Rabbit collection, but not
arbitrary SeExpr or every generator/FX type. The IGS oracle matrix covers
straight hairs on flat and wave surfaces. Rabbit `head_A` established two
otherwise undocumented details: NoiseFX evaluates its spatial domain in XGen's
internal length units, and `$cLength` uses a fixed `2*N+4`-interval
`SgCurve::length` approximation rather than the control polygon. Rabbit `mm`
subsequently validated XPD3 guide records, PTEX guide-ID maps, two ordered
clump levels, guide-local noise, NoiseFX, and CutFX.

After root and local-guide calibration, all nine Rabbit descriptions now run
without fallback and exactly match Maya's per-description topology and
canonical identities. Five pass the strict `1e-3` maximum-position gate; over
the complete 47,421,673-point output, 1,479 points exceed it (0.00312%), with
about `6.39e-5` aggregate RMS and a `0.243994` worst outlier. The residual
comes from rare ill-conditioned frame transport amplifying Maya-double versus
runtime-float differences. These numbers bound the current implementation;
they are not a claim of arbitrary Classic-package compatibility.

## CPU and renderer-payload performance snapshot

Measured in one shared container with 9 logical CPUs, optimized builds, 12 CVs
per strand, and median times. Host scheduling and frequency introduce run-to-run
variation, so these numbers describe this machine rather than a universal
speedup.

| Curves | NanoXGen direct renderer output | NanoXGen generation + generic pack | XGen already-BLOB load + execute + materialize | Minimal `.nxc` read + full validation | Maya invalidated export |
|---:|---:|---:|---:|---:|---:|
| 10,000 / 9,970 XGen | 0.956 ms | 1.123 ms | 10.940 ms | 0.789 ms | 355.908 ms |
| 100,000 / 99,929 XGen | 7.049 ms | 9.209 ms | 29.889 ms | 8.1–9.5 ms | 558–562 ms |

Direct renderer output writes `float4(position, radius)` during generation and
avoids the separate position/width intermediate. The generic pack remains for
variable CV counts, transforms, resampling, primvar merging, and owning 64K
batches. XGen already-BLOB ingestion is still not an identical algorithm
comparison: it parses a proprietary, more general result while NanoXGen
implements fewer authoring features.

Those rows have noise disabled. With the verified gradient-noise core enabled
at magnitude 0.043, frequency 3.17, mask 0.83, correlation 35%, and
`preserveLength` 40%, 100,000 strands / 1.2 million CVs took 68.8 ms for direct
renderer output on the same 9-logical-CPU container. Length preservation needs
an original-length pass, a noisy-length pass, and a final output pass; each CV
also performs three eight-corner gradient samples. This CPU result is still far
below Maya's roughly 550 ms serialization stage, but it is slower than decoding
an already-produced XGen BLOB. The current device path records the supported
passes once through LuisaCompute and dispatches them through HIP or Vulkan.

### Historical cache-residency measurement

The following numbers predate the removal of NanoXGen's handwritten HIP path.
They are retained only as a historical cache-residency observation and are not
evidence for the current Luisa implementation or a cold-start comparison.

The complete local Rabbit Classic example contains nine descriptions. Public
XGen RenderAPI evaluation produced 2,456,139 curves / 47,421,673 CVs; evaluating
the descriptions took 155,119.249 ms in aggregate and writing their nine
renderer-minimal caches took 849.445 ms. The caches total 798,046,252 bytes.
Those Autodesk stages are CPU authoring/evaluation work and are not accelerated
by the device traversal.

The same nine `.nxc` files were then measured together with three warm-ups and
15 repetitions. Their renderer point-count and `float4` sections occupy
768,571,324 device bytes. Hot CPU read plus complete cache validation was
236.1035 ms median / 237.4748 ms p90. The benchmark-only one-time concatenation
was 59.7063 ms. H2D was 13.9142 / 13.9302 ms by device events and
13.9287 / 13.9449 ms by wall time. A GPU traversal that validated all 47.4
million points, summed topology, and matched the host bit checksum
`0x2c58cba9778dc4a2` took 1.2428 / 1.2529 ms.

The H2D stage is a cache admission cost and should not be paid per frame; a
renderer should retain unchanged `.nxc` data on the GPU. The checksum kernel is
a full-buffer correctness and memory-throughput diagnostic, not a render or a
procedural Rabbit-generation time. It therefore remains separate from both the
155-second Autodesk evaluation and NanoXGen native procedural generation.

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

For unchanged assets, `nanoxgen_xgen_cache` independently reads the evaluated
BLOB and writes `.nxc`; Autodesk is not present in this conversion process. A
99,929-curve renderer-minimal cache was 19.6 MB, preserved
all public position/radius floats bit-for-bit, and read plus fully validated in
8.1–9.5 ms. The full 31.2 MB version also preserves texcoords, patch UVs, face
UVs, and face IDs and validated in about 14.2 ms. Runtime loading has no Autodesk
dependency. This is exact evaluated-geometry parity, not procedural algorithm
parity.

The standalone reader was checked on a one-group 244-curve fixture and an
eight-group 99,929-curve fixture. Curve/CV counts, bounds, and canonical hashes
matched `XgFnSpline` exactly. NanoXGen then rebuilt those arrays into new
10-group and seven-group BLOBs respectively; Autodesk loaded both and returned
the same canonical hashes. A 4,096-curve BLOB generated directly from native
NanoXGen procedural output passed the same reverse interoperability check.

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
weight power, verified gradient noise, 35% correlation, and 40% length
preservation. Across 31 single-thread repeats, fast math reduced the median from
90.301 to 88.372 ms (2.1%) on this CPU. It kept root triangle indices exact,
introduced no non-finite values, and produced:

| Array | Maximum absolute error | RMS error |
|---|---:|---:|
| positions | `9.95397568e-5` | `4.74876137e-7` |
| widths | `1.86264515e-9` | `9.18501397e-10` |
| root float attributes | `9.53674316e-7` | `1.03651435e-7` |

This passed the fixture's relaxed, scene-scale-specific production budget but
did not pass strict XGen parity: 10,418 of 960,000 position components were
outside `(5e-7 absolute OR 4 ULP)`. The large maximum is localized: 83
components on three strands exceeded `4e-6` because strict and fast arithmetic
placed their noisy-length delta on opposite sides of XGen's `1e-4` activation
threshold for length preservation. The precision gate therefore limits both
the number of these discontinuities and their `1.1e-4` hard maximum, in addition
to the global RMS bound. Fast math remains opt-in and must be tested separately
on every target compiler/backend before enabling relaxed floating-point math.

One benchmark pitfall was found while adding renderer materialization:
`XgItSpline::vertexCount()` must be cached once per iterator batch. Calling it
inside the per-primitive loop inflated a 1.2-million-CV copy from milliseconds
to seconds. The benchmark now separates iterator metadata access from the
actual renderer-buffer copy.

## Next implementation phases

1. Finish production renderer adoption of the external-Device Classic path,
   including 1--20-sample transport where the current consumer stores only two
   samples, lifetime/invalidation policy, and evaluated `.nxc` fallback.
2. Expand licensed fixtures to capture authored guide inputs and individual
   modifier outputs, not only evaluated final curves.
3. Replace the isotropic support kernel with UV/geodesic, direction-dependent
   guide regions and validate interpolation error against those fixtures.
4. Evaluate moving the currently host-sampled scalar PTEX input tables to
   renderer-owned texture/bindless resources when that reduces cold-start host
   work without changing XGen point-filter semantics.
5. Implement remaining modifier passes in authored dependency order. Primitive
   length/width/taper/ramp, reparameterized Cut, NoiseFX, and hierarchical
   CPU/Luisa clumping are represented; add coil, collision, and wind. Fuse
   passes where locality permits and use explicit intermediate curve buffers
   where modifiers need neighborhoods.
6. Expand Luisa HIP/Vulkan device coverage beyond the tested RX 9070 XT and
   compare throughput, memory, determinism, and strict/float-mode error
   against the XGen CPU baseline.

## Interoperability implementation rule

Algorithm work may use Autodesk's public documentation, public SDK surface,
controlled input/output comparisons, and source files shipped to licensed Maya
users as interoperability references. Do not redistribute or copy Autodesk
implementation code, private headers, or depend on undocumented in-memory
layouts. Implement behavior independently and retain third-party notices for
any separately licensed data. Differential fixtures store our own inputs and
numeric outputs, not Autodesk binaries.

## Public references

- Autodesk Maya API/DevKit downloads:
  <https://aps.autodesk.com/developer/overview/maya-api>
- Autodesk's XGen Interactive Grooming SDK example and `XgFnSpline` layout:
  <https://blog.autodesk.io/support-xgen-interactive-grooming-feature-in-your-plugin/>
- Autodesk's documented guide neighborhood and interpolation process:
  <https://download.autodesk.com/global/docs/maya2014/en_us/files/GUID-49B8C299-71DD-4341-8638-97437D9A328F.htm>
- Autodesk's noise frequency, correlation, and length-preservation documentation:
  <https://help.autodesk.com/cloudhelp/2017/CHS/Maya/files/GUID-968ACF77-A9CC-4BF1-9CE0-73C1B4EB06F7.htm>
- Autodesk's noise magnitude and magnitude-scale documentation:
  <https://help.autodesk.com/cloudhelp/2017/CHS/Maya/files/GUID-F6E70334-5E7D-4393-9D07-F675A08A9695.htm>
- Autodesk's XGen file inventory:
  <https://download.autodesk.com/global/docs/maya2014/en_US/files/GUID-66DECD58-3AEC-46B1-8CC7-FD968B60B044.htm>
- Thompson, Petti, and Tappan, *XGen: Arbitrary Primitive Generator*:
  <https://forums.autodesk.com/autodesk/attachments/autodesk/maya-ideas-en/2810/2/XGen%20Arbitrary%20Primitive%20Generator.pdf>
- MIT-licensed `XgFnSpline` to CyHair reference converter:
  <https://github.com/syoyo/xgen-spline-to-cyhair>
