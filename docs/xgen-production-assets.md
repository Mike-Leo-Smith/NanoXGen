# Production XGen assets

NanoXGen distinguishes authoring assets from evaluated renderer data. The
`.xgen` suffix is ambiguous: Classic XGen uses it for a text collection, while
the calibration tools in this repository use it for a serialized Interactive
Grooming `XgSplineData` renderer snapshot. Classification therefore checks file
content rather than the suffix alone.

## Support boundary

| Asset layer | Core-only handling | Evaluation backend |
|---|---|---|
| Evaluated `XgSplineData` BLOB | Parse, validate, preserve, process, pack, cache | Native |
| `.nxc` evaluated curve cache | Validate and consume | Native |
| Classic collection/description package | Inventory and text-reference closure | Autodesk Classic typed bridge required for arbitrary procedural evaluation |
| Interactive Groom authoring state in `.ma/.mb` | File inventory only; Maya DG owns the dependency graph | Maya/Autodesk required |
| PTEX, CAF, XUV/XPD/XGC, Alembic, archives, textures and plugins | Typed inventory and dependency reporting | Depends on the owning Classic/Interactive graph |

The core does not claim to reproduce arbitrary SeExpr, PTEX lookup, custom
modules, archive/card/sphere primitives, or Interactive Groom modifier graphs.
Those cases must select an Autodesk backend and cache the evaluated result.

## Package inspection

```bash
./build/release/nanoxgen_xgen_package \
  --var DESC=/show/asset/xgen/collections/fur/description \
  --require-complete /show/asset
```

The command returns JSON containing the inventory, references, closure status,
diagnostics, and the minimum evaluation backend. `${PROJECT}` defaults to the
package root. Other variables are explicit so the scanner cannot silently bind
a dependency to the wrong studio directory.

The scanner:

- recognizes Classic collection/description/delta/FX files, Interactive
  presets and Maya scenes, evaluated BLOBs and caches, CAF/Alembic/PTEX,
  XUV/XPD/XGC, archives, textures, and scripts/plugins;
- resolves `${PROJECT}` and caller-provided variables, including the common
  `${PROJECT}xgen/...` concatenated spelling;
- reports resolved, missing, package-external, unresolved-variable, and unsafe
  references;
- inventories but never follows symbolic links;
- enforces file-count and per-text-file limits;
- marks Maya/Interactive dependency closure incomplete because the Maya DG must
  be enumerated by a Maya-side bridge.

`--require-complete` exits with status 3 when the core cannot prove the closure.

## Evaluated snapshot ingestion

Static rendering can preserve source emission order and skip canonical surface
sorting:

```bash
./build/release/nanoxgen_xgen_cache \
  --renderer-minimal --source-order snapshot.xgen snapshot.nxc
```

Motion samples and reproducible cross-evaluation caches must keep canonical
face/UV ordering. The existing default remains canonical for compatibility.

Benchmark the stages independently:

```bash
./build/release/nanoxgen_xgen_read_benchmark \
  --warmup 5 --iterations 30 snapshot.xgen
```

The JSON result separates file reading, resident document parse/deflate,
source-order renderer packing, canonical renderer packing, full-channel
canonical materialization, and end-to-end hot-file ingestion. Do not compare a
full-channel result with an Autodesk benchmark that only copies point counts and
`float4(position, radius)`.

## Production architecture

1. Scan the package and resolve its dependency closure.
2. Consume existing evaluated BLOB/`.nxc` data natively when valid.
3. For Classic procedural evaluation, receive `PrimitiveCache` typed arrays
   directly from the public RenderAPI; do not serialize an intermediate BLOB.
4. For Interactive Groom, perform only the unavoidable public `MPxData`
   serialization into memory, then decode and cache with NanoXGen; do not write
   a temporary `.xgen` or rematerialize through `XgFnSpline`.
5. Use source order for one-shot static rendering. Use canonical identity order
   only for motion matching and reproducible caches.

The Autodesk bridges are optional build products and must remain absent from the
default core dependency graph. Real production packages are still required to
calibrate PTEX, custom plugin, archive, deformation, motion, and failure paths.

The implemented Classic curve bridge is built only by
`autodesk-bridge-release`. It validates finite positions and U/V, finite
non-negative widths, channel cardinality and topology inside the callback. It
accepts XGen's B-spline varying-width layout (`NumVertices - 2` per curve),
expands endpoint values while writing the final packed buffer, and converts
diameter to radius exactly once. Autodesk palette cleanup is owned by a
single-run guard across success, empty, and error paths.

The Interactive bridge is a Maya plugin rather than an XGen standalone client.
The only Autodesk data conversion is `MPxData::writeBinary` into a vector-backed
memory stream. Resident bytes go directly to the independent NanoXGen decoder
and renderer-minimal `.nxc` builder. Source order avoids identity work for a
static sample; canonical mode uses the exact face/face-UV/patch-UV bit identity
and rejects duplicates. The bridge deliberately has no direct `libAdskXGen`
dependency and does not reload its own serialization through `XgFnSpline`.
