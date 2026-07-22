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
| Interactive Groom BLOB import | `XgFnSpline` validation and canonical summary | validated with Maya 2027.1 |

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
| baseline A | 4 | 1.0 | 0.02 | 5 | 16 | 80 | `0x8b4b5302edbd4227` |
| baseline B | 4 | 1.0 | 0.02 | 5 | 16 | 80 | `0x8b4b5302edbd4227` |
| variant | 2 | 0.5 | 0.03 | 7 | 8 | 56 | `0x7450c27a657d03bb` |

The two baseline exports had different raw file hashes but identical canonical
geometry. Therefore the serialized container contains nondeterministic bytes,
while the tested positions, widths, curve texture coordinates, and patch UVs
were deterministic. The variant also confirmed that density, length, width, and
CV-count changes propagate through the real runtime as expected. An empty BLOB
was rejected by `XgFnSpline` with an invalid-stream error.

## Next implementation phases

1. Expand the licensed fixture set to capture guide interpolation, motion
   samples, face bindings, and individual modifier outputs.
2. Replace the isotropic support kernel with UV/geodesic, direction-dependent
   guide regions and validate interpolation error against those fixtures.
3. Compile a restricted XGen-expression subset to a typed GPU bytecode/IR and
   add PTEX sampling through a texture indirection table.
4. Implement modifier passes in dependency order: cut/length, clump, noise,
   coil, collision, and wind. Fuse passes where locality permits and use
   explicit intermediate curve buffers where modifiers need neighborhoods.
5. Add CUDA and LuisaCompute backends, then compare throughput, peak memory,
   determinism, and geometry error against the XGen CPU baseline.

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
