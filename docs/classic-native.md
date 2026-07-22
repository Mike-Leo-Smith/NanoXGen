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
only the face IDs declared by the description, triangulates polygon faces,
evaluates embedded guide roots, transforms relative guide CVs, and passes the
result through normal `.nxg` validation. Missing or ambiguous patch objects,
bad topology, non-finite values, invalid guide ranges, and limit overflows are
checked errors.

On the local Rabbit package, all nine descriptions imported successfully. The
`mm` description contains 188 source vertices, 152 selected quad faces, 366
guides, and 1,830 guide CVs; its generated `.nxg` is about 65 KB. These numbers
describe authoring input, not evaluated curve output.

## Current parity boundary

This first importer evaluates a guide root on the Alembic quad cage with
bilinear coordinates and triangulates the same cage for root sampling. Classic
`Subd` evaluation uses a subdivision limit surface, so cage roots are not yet a
bit-exact substitute. The current `.nxg` generator also uses NanoXGen root RNG
and guide neighborhoods. Scalar/ramp IR is independently JIT-capable, but PTEX,
the full expression environment, and authored FX-module ordering are not yet
wired into this asset builder.

Consequently, timings from this path are engineering measurements for the
native prototype, not an equal-output Maya speedup claim. A valid Maya
comparison requires the same roots, surface evaluation, expressions, PTEX,
modules, curve counts, CV counts, and renderer channels.
