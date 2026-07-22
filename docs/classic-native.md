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
per-face primitive IDs, exact SeExpr random prefixes, and directional guide
association. Classic guide CVs are interpreted in XGen's local
`(tangent, normal, binormal)` patch frame and uniformly rebuilt before blending.

On the local Rabbit `eyelash` description this produces 1519 roots and, after
NoiseFX/CutFX culling and renderer endpoints, exactly 1514 curves and 25738
points like Maya, with no fallback, and stays within the recorded geometry
tolerance. `head_A` lowers all four scalar/Noise/Cut passes with no fallback and
matches Maya's 307791-curve/5232447-point topology. Matching XGen's internal
Noise coordinate units and fixed `SgCurve::length` sampling reduced its full
CPU result to about `4.13e-6` RMS position error, with a `3.16e-3` maximum on a
subdivision-boundary strand. It therefore remains outside the strict `1e-3`
maximum-error gate. Rabbit `mm` lowers both ClumpingFX modules, the active
NoiseFX module, and CutFX with zero CPU fallback and matches Maya's complete
3526-curve/59942-renderer-point topology. Against a fresh typed RenderAPI
cache, the full result has `2.62e-3` maximum and `1.23e-4` RMS position error;
the two-clump intermediate has `1.53e-3` maximum and `1.09e-4` RMS error.
Rabbit `body` and `head` now lower their PTEX-backed primitive/FX expressions
without fallback. Rabbit `nose` also lowers its palette scalar function, but
produces 67337 native strands versus 67231 in the Maya cache. These are not yet
parity claims: `body` produces 330101 native strands versus 330038 in the
current Maya cache, and the deeper multi-effect Luisa differential still has a
`3.07e-2` maximum position error. `erduo` retains a position-vector `noise`
expression fallback; several zero-fallback descriptions retain known
topology/geometry mismatches.

## Current parity boundary

The OpenSubdiv path evaluates current and reference limit patches and retains
stable coarse-face identities, but boundary/crease metadata is absent when an
Alembic file stores the Maya patch as a plain `PolyMesh`. PTEX density masks
are bound for RandomGenerator, point-filtered encoded guide maps are bound for
hierarchical ClumpingFX, and runtime scalar `map()` inputs are pre-sampled into
float tables. No-argument palette scalar functions are compiled into per-root
inputs. Position-vector SeExpr noise, the remaining advanced ClumpingFX
controls, and several strict topology/geometry differentials are the current
Rabbit-wide parity boundary.

Consequently, timings from this path are engineering measurements for the
native prototype, not an equal-output Maya speedup claim. A valid Maya
comparison requires the same roots, surface evaluation, expressions, PTEX,
modules, curve counts, CV counts, and renderer channels.

The optional Luisa path uploads rebuilt float guides, CSR associations, and
bound clump guide axes/maps, generates final packed base points, and runs the
same float expression, ClumpingFX, NoiseFX, CutFX, and width kernels through
HIP or Vulkan. See
`docs/luisa-compute.md` for the cold/no-cache Rabbit command and its explicit
parity boundary.
