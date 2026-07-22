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
```

The output path must stay outside the repository for production assets. The
importer reads sample zero, applies public Alembic parent transforms, selects
only the face IDs declared by the description, and transforms relative guide
CVs. `Subd` patches use OpenSubdiv Catmull-Clark limit patches for guide
positions/derivatives and a bounded per-face tessellation for root sampling;
polygon patches use their cage directly. The result passes normal `.nxg`
validation. Missing or ambiguous patch objects, bad topology, non-finite
values, invalid guide ranges, and limit overflows are checked errors.

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
`taperStart`, and `widthRamp`, plus mode-0 `NoiseFXModule` and
`CutFXModule` in authored order. Noise uses the SeExpr gradient table and
XGen's transported surface frame. `rebuildType=1` cuts use the cubic
`SgCurve::cutFromTip` search/rebuild convention, including fully-cut culling.
Width evaluation stores half the final diameter in renderer
`float4.radius`, and the renderer output adds XGen's two extrapolated endpoint
CVs. Unsupported variables, unbound PTEX expressions, keep-param cuts, and
other active FX modules produce `fallback_reasons`; an unknown binding is
never silently replaced by zero.

The root planner reproduces RandomGenerator face counts, the fixed XGen sample
table, surface compensation, mask ramp, current/reference OpenSubdiv samples,
per-face primitive IDs, exact SeExpr random prefixes, and directional guide
association. Classic guide CVs are interpreted in XGen's local
`(tangent, normal, binormal)` patch frame and uniformly rebuilt before blending.

On the local Rabbit eyelash description this produces 1519 roots and, after
NoiseFX/CutFX culling and renderer endpoints, exactly 1514 curves and 25738
points like Maya, with no fallback, and stays within the recorded geometry
tolerance. Seven other descriptions retain explicit ClumpingFX,
PTEX-backed primitive length/width, or unsupported-expression fallbacks.
`head_A` currently lowers all of its scalar/Noise/Cut passes and matches Maya's
307791-curve/5232447-point topology, but its first NoiseFX stage fails the
geometry oracle; it is therefore not treated as native-compatible.

## Current parity boundary

The OpenSubdiv path evaluates current and reference limit patches and retains
stable coarse-face identities, but boundary/crease metadata is absent when an
Alembic file stores the Maya patch as a plain `PolyMesh`. PTEX density masks are
bound for RandomGenerator. PTEX-backed primitive/FX attributes, ClumpingFX,
and the remaining SeExpr method/operator forms are the current Rabbit-wide
parity boundary.

Consequently, timings from this path are engineering measurements for the
native prototype, not an equal-output Maya speedup claim. A valid Maya
comparison requires the same roots, surface evaluation, expressions, PTEX,
modules, curve counts, CV counts, and renderer channels.

The optional Luisa path uploads rebuilt float guides and CSR associations,
generates final packed base points, and runs the same float expression,
NoiseFX, CutFX, and width kernels through HIP or Vulkan. See
`docs/luisa-compute.md` for the cold/no-cache Rabbit command and its explicit
parity boundary.
