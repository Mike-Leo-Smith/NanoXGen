# Autodesk Maya/XGen SDK setup

## Maya API-only setup

Run:

```bash
./scripts/fetch_maya_devkit.sh
```

This downloads the official Maya 2026 Update 3 Linux DevKit and verifies SHA-256
`d23cc9e788a0114c683983363e28b08f040b900ab728bbf4707baee2dc563c37`.

The DevKit does **not** contain XGen. It is sufficient for Maya API plugin
compilation, but not for Interactive Grooming integration tests.

## Real XGen runtime setup

Install a matching full Maya build through your Autodesk entitlement. Maya
2027.1 provides the public spline API at
`plug-ins/xgen/include/xgen/src/xgsculptcore/api/XgSplineAPI.h` and the runtime
library at `plug-ins/xgen/lib/libAdskXGen.so`.

Point NanoXGen to the full installation:

```bash
export MAYA_LOCATION=/usr/autodesk/maya2027
export XGEN_ROOT="$MAYA_LOCATION/plug-ins/xgen"
./scripts/check_xgen_sdk.sh "$XGEN_ROOT"
cmake --preset calibration-release -DXGEN_ROOT="$XGEN_ROOT"
cmake --build --preset calibration-release
```

Only the explicit calibration preset discovers XGen and builds
`nanoxgen_xgen_probe`, `nanoxgen_xgen_parity`, and the independent-converter
oracle. The default release/debug/native presets build the self-contained
`.xgen` parser/writer/converter and never inspect LuisaCompute or Maya, or link Autodesk
libraries. Create an Interactive Grooming fixture in Maya and
export its serialized render data with:

```mel
xgmExportSplineDataInternal -output "/tmp/fixture.xgen" "description1_Shape.outRenderData";
```

Then run:

```bash
LD_LIBRARY_PATH="$XGEN_ROOT/lib:${MAYA_LOCATION}/lib:$LD_LIBRARY_PATH" \
  ./build/calibration-release/nanoxgen_xgen_probe /tmp/fixture.xgen
```

The probe reports motion samples, batches, curve and vertex counts, CV ranges,
widths, position and patch-UV bounds, and a canonical geometry hash. The
standalone NanoXGen core does not need Maya or XGen at build or runtime. The
calibration probe uses `XgFnSpline` only to verify the independent decoder and
writer against a licensed reference implementation.

### Classic typed renderer bridge

The separate `autodesk-bridge-release` preset builds the Classic curve bridge.
It uses the public `XGenRenderAPI::PatchRenderer` and `FaceRenderer` callbacks,
reads `PrimitiveCache` point counts, positions, widths, U/V and face IDs, and
writes final `float4(position, radius)` values directly. XGen widths are full
diameters; the cache stores half-width radii. It does not create an evaluated
Interactive `.xgen` BLOB.

```bash
cmake --preset autodesk-bridge-release \
  -DXGEN_ROOT="$XGEN_ROOT" \
  -DNANOXGEN_COMPAT_LIB_DIR="${NANOXGEN_COMPAT_LIB_DIR:-}"
cmake --build --preset autodesk-bridge-release

./scripts/run_xgen_classic_typed_test.sh \
  --xgen-file /outside/repository/collection.xgen \
  --palette collection_name \
  --geom /outside/repository/patches.abc \
  --patch patch_name \
  --description description_name
```

The script keeps its generated `.nxc` under ignored `build/` output unless an
external `--output` is given, verifies runtime linkage with `ldd`, executes the
typed bridge, and reads the cache back through the Autodesk-free validator.
Missing dependencies reported by the collection remain XGen errors; a missing
patch or empty result is a checked bridge failure rather than a crash. Classic
archive/card/sphere primitives are outside this curve-only bridge.

### Interactive in-memory cache bridge

The same explicit preset builds `nanoxgen_maya_xgen_cache.so`. Its
`nanoxgenXGenCache` Maya command evaluates an Interactive Groom description's
`outRenderData`, invokes the public `MPxData::writeBinary` exactly once into an
in-memory stream, and immediately passes those resident bytes to NanoXGen. The
command never writes a temporary `.xgen` BLOB and never invokes
`XgFnSpline::load`, `executeScript`, or materialization.

```python
cmds.loadPlugin("/path/to/nanoxgen_maya_xgen_cache.so")
cmds.nanoxgenXGenCache(description, "/cache/groom.nxc", "source-order")
cmds.nanoxgenXGenCache(description, "/cache/reproducible.nxc", "canonical")
```

`source-order` is the default for a one-shot static render or cache. Canonical
mode sorts by the exact five-word identity `(faceId, bit(faceUV.x),
bit(faceUV.y), bit(patchUV.x), bit(patchUV.y))` and rejects duplicate
identities; use it for repeatable cross-evaluation caches and motion matching.
Only dirty samples should call Maya again. Repeated rendering should consume
the resulting `.nxc` through the Autodesk-free runtime.

Run the real Maya-side differential test with:

```bash
./scripts/run_maya_xgen_cache_test.sh
```

It creates an Interactive Groom in Maya, exercises both order modes, exports a
separate disk BLOB only as a test oracle, and requires the two in-memory caches
to be byte-identical to their standalone conversions. It also checks that the
plugin itself does not link `libAdskXGen`. All generated BLOBs, caches, and JSON
remain under ignored `build/` output.

For the complete reproducible test, run:

```bash
export MAYA_LOCATION=/usr/autodesk/maya2027
./scripts/run_xgen_real_tests.sh
```

This compiles the probe, launches `mayapy` with Maya Standalone, creates two
independent baseline grooms, a parameter variant, and four subdivided wave-mesh
fixtures. It exports real Interactive Grooming render data, checks the BLOB
through `XgFnSpline`, canonicalizes curves by face/root coordinates, validates a
noise/cut chain, and compares NanoXGen's linear base/cut/taper path against the
official output attribute by attribute. Malformed input must also be rejected.
Outputs are written under `build/xgen-real/` and are ignored by Git.

For a staged performance comparison, run:

```bash
export MAYA_LOCATION=/usr/autodesk/maya2027
./scripts/run_xgen_benchmark.sh
```

The output separates NanoXGen in-memory generation, Maya lazy-plug access/export, and
the public API's BLOB load/execute/iteration stages. The Maya export number
includes serialization and fixed host overhead and is not an apples-to-apples
kernel timing.

To identify modifier semantics through controlled public input/output tests,
run `scripts/run_xgen_modifier_study.sh`. It also writes
`modifier-schema.json` from Maya's public node attributes, avoiding guessed
clump/coil/collision parameter names before those fixture families are added.

To profile the exact renderer bridge (`asMObject`, `writeBinary`, load,
execute, and materialization) inside Maya, run:

```bash
./scripts/run_xgen_bridge_profile.sh
```

The default CMake build provides `nanoxgen_xgen_cache`; XGen is not required to
convert evaluated output to an Autodesk-free runtime cache. Motion samples are
separate evaluations and are canonicalized before storage:

```bash
./build/release/nanoxgen_xgen_cache --renderer-minimal \
  --motion 0.5 /tmp/shutter-close.xgen /tmp/base.xgen /tmp/groom.nxc
./build/release/nanoxgen_cache_benchmark --repeats 15 /tmp/groom.nxc
```

Maya's supported RHEL/Rocky systems should provide the required libraries. On
another Linux distribution, a workspace-local compatibility directory can be
appended without modifying the system:

```bash
export NANOXGEN_COMPAT_LIB_DIR=/path/to/rocky-compatible/usr/lib64
./scripts/run_xgen_real_tests.sh
```

The test is intentionally excluded from public CI: a full licensed Maya/XGen
installation is proprietary and must not be committed or redistributed. A
headless run can print `This plugin does not support
createPlatformOpenGLContext!`; this did not affect Maya Standalone fixture
generation or export in the tested Maya 2027.1 setup.
