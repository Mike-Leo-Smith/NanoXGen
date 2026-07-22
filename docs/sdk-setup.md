# Autodesk SDK setup

Run:

```bash
./scripts/fetch_maya_devkit.sh
```

This downloads the official Maya 2026 Update 3 Linux DevKit and verifies SHA-256
`d23cc9e788a0114c683983363e28b08f040b900ab728bbf4707baee2dc563c37`.

The DevKit does **not** contain XGen. Install a matching Maya build through your
Autodesk entitlement, then point NanoXGen to the XGen directory:

```bash
export XGEN_ROOT=/usr/autodesk/maya2026/plug-ins/xgen
./scripts/check_xgen_sdk.sh "$XGEN_ROOT"
cmake -S . -B build -DXGEN_ROOT="$XGEN_ROOT"
```

With the SDK present, CMake builds `nanoxgen_xgen_probe`. Create an Interactive
Grooming fixture in Maya and export its serialized render data with:

```mel
xgmExportSplineDataInternal -output "/tmp/fixture.xgen" "description1_Shape.outRenderData";
```

Then run:

```bash
LD_LIBRARY_PATH="$XGEN_ROOT/lib:${MAYA_LOCATION}/lib:$LD_LIBRARY_PATH" \
  ./build/nanoxgen_xgen_probe /tmp/fixture.xgen
```

The probe reports motion samples, batches, curves, and vertices. It is the first
step toward a differential harness; the standalone NanoXGen core does not need
Maya or XGen at runtime.
