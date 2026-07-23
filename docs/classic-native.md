# Native Classic input pipeline

NanoXGen has an explicit, optional input stage for turning a parsed Classic
description plus its Alembic patch sample into a compact `.nxg` generation
asset. This stage uses the system Alembic, OpenSubdiv, and Ptex libraries; it
does not link Maya or XGen, and it is disabled in every default CPU/GPU preset.

```bash
cmake --preset classic-alembic-release
cmake --build --preset classic-alembic-release
ctest --preset classic-alembic-release

./build/classic-alembic-release/nanoxgen_xgen_classic_asset \
  --description mm \
  --output /external/generated/rabbit-mm.nxg \
  /external/rabbit/yxt_rabbit__yxt_test_fur.xgen \
  /external/rabbit/yxt_rabbit__yxt_test_fur.abc

./build/classic-alembic-release/nanoxgen_xgen_ptex_inspect \
  /external/rabbit/xgen/collections/collection/description/paintmaps/map/patch.ptx

./build/release/nanoxgen_xpd_inspect \
  /external/rabbit/xgen/collections/collection/description/Clumping1/Points/patch.xuv
```

The output path must stay outside the repository for production assets. The
importer reads sample zero, applies public Alembic parent transforms, selects
only the face IDs declared by the description, and transforms relative guide
CVs. `Subd` patches use OpenSubdiv Catmull-Clark limit patches for guide
positions/derivatives and a bounded per-face tessellation for root sampling;
polygon patches use their cage directly. The result passes normal `.nxg`
validation. Missing or ambiguous patch objects, bad topology, non-finite
values, invalid guide ranges, and limit overflows are checked errors.

The default core also contains an independent, bounds-checked XPD3 reader for
XPD/XUV block metadata and little-endian float records. It does not link
`libAdskXpd`. On Rabbit `mm/Clumping1`, the native inspector and Autodesk's
public `XpdReader` agree on all 4,040 six-float `Location` records. Reading the
point file is only the input layer; a package is not declared native-compatible
until the owning Clumping evaluation is lowered as well. The optional Classic
native target binds ClumpingFX modules directly from their XPD3 `Location`
blocks and encoded PTEX guide-ID maps. It generates guide axes from the same
explicit patch roots, recursively applies upstream clumps for hierarchical
maps, and the backend-neutral CPU runtime applies float clump and guide-noise
expressions without Autodesk or an intermediate renderer BLOB.

The same preset builds a float-only, bounds-checked facade over the system Ptex
reader. It exposes face metadata, normalized face/UV filtering, and unpacked
channel texels for later CPU/GPU root and expression planning. Ptex decoding
is not part of the default core or the GPU hot path. On the local Rabbit body
density map it reads 12,906 quad faces, three normalized `uint8` channels,
28,434,624 logical texels, and values spanning `[0, 1]`.

On the local Rabbit package, all nine descriptions imported successfully. The
`mm` description contains 188 source vertices, 152 selected quad faces, 366
guides, and 1,830 guide CVs. At the default two-segment limit tessellation its
generated `.nxg` is about 142 KB. Its 366 limit-surface guide roots differ from
the old bilinear cage approximation by 0.02119 scene units RMS and 0.06720
maximum on this asset. These numbers describe authoring input, not evaluated
curve output.

## Float runtime plan

`compile_xgen_classic_float_runtime_plan` converts supported authoring
attributes once into `XgenFloatExpressionProgram`. The retained plan and its
CPU/Luisa execution values use only `float` and `uint32`; the lossless Classic
parser and Autodesk/SeExpr calibration oracle keep their higher-precision
source representation outside the render hot path.

The current plan applies SplinePrimitive `length`, `width`, `taper`,
`taperStart`, and `widthRamp`, plus `ClumpingFXModule`, mode-0
`NoiseFXModule`, and `CutFXModule` in authored order. Clumping supports ordered
guide hierarchies, guide-space noise/frequency/correlation/ramp expressions,
and control grouping already encoded by the point and guide-ID maps. Authored
clump variance/cut/copy/curl/offset/flatness/volumizing controls remain
explicit fallbacks. Noise uses the SeExpr gradient table and
XGen's transported surface frame. `rebuildType=1` cuts use the cubic
`SgCurve::cutFromTip` search/rebuild convention, including fully-cut culling.
`map()` calls in these scalar attributes are assigned stable runtime inputs.
The optional PTEX stage expands `${DESC}`, point-samples each map at the
strand's coarse-face `(face,u,v)` identity, and produces a row-major float
table shared by CPU and Luisa. PTEX objects and paths never enter the core or
the GPU kernel. Width evaluation stores half the final diameter in renderer
`float4.radius`, and the renderer output adds XGen's two extrapolated endpoint
CVs. Palette `custom_float_NAME` functions used without arguments are compiled
once and evaluated into the same per-root float table; the Rabbit `nose`
`long()->gamma(2)->contrast(0.9)` chain is covered. XGen clamps a negative
authored width/profile result to zero before renderer output, while the typed
bridge continues to reject a negative width received from Autodesk as a
corrupt cache. Unsupported variables, vector expressions, keep-param cuts, and
other active FX modules produce `fallback_reasons`; an unknown binding is
never silently replaced by zero.

The root planner reproduces RandomGenerator face counts, the fixed XGen sample
table, surface compensation, mask ramp, current/reference OpenSubdiv samples,
per-face primitive IDs, Patch-authored `culledPrims`, exact SeExpr random
prefixes, and directional guide association. Patch exclusions are applied at
the same post-mask, pre-interpolation point as `XgPatch::isCulled`, preserving
the observable gaps in `$id`. Classic guide CVs are interpreted in XGen's local
`(tangent, normal, binormal)` patch frame and uniformly rebuilt before blending.

The complete local Rabbit collection now lowers all nine descriptions with no
Autodesk fallback. Native CPU, Luisa HIP, and Luisa Vulkan produce the same
2,456,139-curve / 47,421,673-point collection topology as fresh Maya typed
RenderAPI captures. Per-description curve/point counts and canonical
`(faceId, faceUV, patchUV)` identities are exact. The following geometry
figures are from a fresh HIP run compared in canonical order with those Maya
`.nxc` captures:

| description | curves | points | max position error | RMS position error | points > `1e-3` |
| --- | ---: | ---: | ---: | ---: | ---: |
| `mm` | 3,526 | 59,942 | `2.29e-5` | `2.12e-6` | 0 |
| `erduo` | 339,574 | 5,772,758 | `8.53e-2` | `1.58e-4` | 1,005 |
| `body` | 330,038 | 8,911,026 | `2.44e-1` | `7.12e-5` | 132 |
| `eyelash` | 1,514 | 25,738 | `1.59e-4` | `1.18e-5` | 0 |
| `head` | 1,150,937 | 19,565,929 | `3.60e-4` | `2.50e-6` | 0 |
| `nose` | 67,231 | 1,142,927 | `2.17e-3` | `2.70e-6` | 3 |
| `sizhi` | 236,693 | 6,390,711 | `2.34e-2` | `2.76e-5` | 339 |
| `weiba` | 18,835 | 320,195 | `1.80e-4` | `3.64e-6` | 0 |
| `head_A` | 307,791 | 5,232,447 | `1.80e-4` | `2.59e-6` | 0 |

Five descriptions pass the strict `1e-3` maximum-position gate. Across the
whole collection, 1,479 points exceed it, or 0.00312% of all renderer points;
the point-weighted RMS is about `6.39e-5`. `body` stage 7 is the last NoiseFX
stage, not the final stage: its rare near-reversing curves reach `0.11728`
maximum error there, and final stage 8 CutFX amplifies the maximum to
`0.243994`. The dominant residual is the amplification of small float/double
differences in ill-conditioned transported frames. NanoXGen's GPU runtime
remains float-only; the double implementation is confined to host-side
Autodesk calibration.

## Current parity boundary

The OpenSubdiv path evaluates current and reference limit patches and retains
stable coarse-face identities, but boundary/crease metadata is absent when an
Alembic file stores the Maya patch as a plain `PolyMesh`. PTEX density masks,
point-filtered encoded guide maps, runtime scalar `map()` inputs, no-argument
palette functions, and the Rabbit `$Prefg` noise binding are pre-sampled into
typed float tables. General vector SeExpr, uncalibrated ClumpingFX controls,
other generator/FX types, and the strict geometry outliers above remain the
native-compatibility boundary.

The full-Rabbit timing comparison is valid for renderer position/radius work:
all measured paths consume the same authoring inputs, run the same active
modules, and produce exact curve/CV counts and canonical identities. It is
still an engineering result for this calibrated asset, not evidence that
arbitrary Classic packages are natively compatible.

The optional Luisa path uploads rebuilt float guides, CSR associations, and
bound clump guide axes/maps, generates final packed base points, and runs the
same float expression, ClumpingFX, NoiseFX, CutFX, and width kernels through
HIP or Vulkan. See
`docs/luisa-compute.md` for the cold/no-cache Rabbit command and its explicit
parity boundary.
