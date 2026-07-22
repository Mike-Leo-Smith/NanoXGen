# Native Classic input pipeline

NanoXGen has an explicit, optional input stage for turning a parsed Classic
description plus its Alembic patch sample into a compact `.nxg` generation
asset. This stage uses the system Alembic library; it does not link Maya or
XGen, and it is disabled in every default CPU/GPU preset.

```bash
cmake --preset classic-alembic-release
cmake --build --preset classic-alembic-release
ctest --preset classic-alembic-release

./build/classic-alembic-release/nanoxgen_xgen_classic_asset \
  --description mm \
  --output /external/generated/rabbit-mm.nxg \
  /external/rabbit/yxt_rabbit__yxt_test_fur.xgen \
  /external/rabbit/yxt_rabbit__yxt_test_fur.abc
```

The output path must stay outside the repository for production assets. The
importer reads sample zero, applies public Alembic parent transforms, selects
only the face IDs declared by the description, and transforms relative guide
CVs. `Subd` patches use OpenSubdiv Catmull-Clark limit patches for guide
positions/derivatives and a bounded per-face tessellation for root sampling;
polygon patches use their cage directly. The result passes normal `.nxg`
validation. Missing or ambiguous patch objects, bad topology, non-finite
values, invalid guide ranges, and limit overflows are checked errors.

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
`taperStart`, and `widthRamp`, followed by active `CutFXModule` expressions in
authored order. `rebuildType=1` cuts resample the remaining arc to the original
fixed CV count. Width evaluation follows the public `SgCurve::calcNormTex`
contract and stores half the final diameter in renderer `float4.radius`.
Unsupported variables, PTEX expressions, keep-param cuts, and other active FX
modules produce `fallback_reasons`; an unknown binding is never silently
replaced by zero.

`nanoxgen_xgen_classic_inspect` reports the compiled plan for each description.
On the local Rabbit collection, all nine primitive profiles compile partially;
the supported Cut count is 1/1/2/1/2/1/0/2/2 in collection order. Every
description still reports fallback because Autodesk-equivalent root sampling
and one or more authored clump/noise/PTEX operations are not complete.

## Current parity boundary

The OpenSubdiv path removes the earlier bilinear-cage guide-root approximation,
but its root sampling still uses a finite limit-surface tessellation (two
segments per coarse-face edge by default), not XGen's own adaptive patch
sampler. Boundary/crease metadata is also absent when an Alembic file stores
the Maya patch as a plain `PolyMesh`. The current `.nxg` generator still uses
NanoXGen root RNG and guide neighborhoods. The supported primitive and Cut
scalar/ramp subset is wired into a float runtime plan and directly JIT-bindable
by LuisaCompute, but PTEX, the full expression environment, clumping, and the
authored noise passes are not yet complete.

Consequently, timings from this path are engineering measurements for the
native prototype, not an equal-output Maya speedup claim. A valid Maya
comparison requires the same roots, surface evaluation, expressions, PTEX,
modules, curve counts, CV counts, and renderer channels.
