# NanoXGen data layout

`AssetHeader` begins a single immutable, relocatable byte blob. Every reference
is a 64-bit byte offset from the blob base; there are no host pointers, virtual
functions, strings, or owning containers. Sections are at least 16-byte aligned.

The v0.1 sections are:

1. vertex positions, normals, and optional UVs;
2. triangle indices;
3. Walker alias entries weighted by rest-pose triangle area;
4. guide records and a packed guide-CV array;
5. a fixed eight-guide candidate stencil per triangle.

The header carries magic/version/size metadata plus an FNV-1a content hash.
`validate_asset` checks all section ranges and cross-references before the blob
is uploaded to a GPU. `DeviceAssetView` is a small non-owning accessor that is
compiled unchanged for C++ and CUDA/HIP device code.

Generated curves use fixed CV counts in v0.1. Points and widths are strand-major,
so a CUDA thread owns one contiguous strand. The CUDA kernel keeps its direct
static mapping. On CPU, a logical work block is a contiguous tile of strands
with the same default width as a CUDA block (128 strands); persistent CPU worker
threads dynamically claim these tiles through a relaxed atomic counter. This
amortizes scheduling and balances future variable-cost modifiers without
changing CUDA behavior. A later backend can transpose CV tiles for
warp-coalesced modifier passes without changing the source asset format.

Frame-local deformation is an overlay rather than a second asset blob. Optional
position, normal, and guide-CV pointers replace the corresponding immutable
sections during generation. The rest-pose alias table and sampled
triangle/barycentric coordinates remain unchanged, which preserves strand
identity across motion samples.

Renderer payloads are deliberately separate from `.nxg`: they own point counts,
aligned `float4(position, radius)` values, root UVs, motion positions, and
face-uniform primvars. Keeping transport data out of the asset avoids baking a
specific renderer ABI into the persistent format.

`.nxc` is a separate evaluated-curve cache, not an authoring/procedural asset.
Its pointer-free header references per-curve counts, aligned
`float4(position, radius)`, optional public XGen attributes, and optional
sample-major motion positions. It exists for exact static/baked fallback:
official `XgFnSpline` output is canonically ordered and stored bit-for-bit, then
loaded without Maya/XGen at render time. A word-at-a-time content checksum,
section bounds, topology sums, finite values, radii, and increasing shutter
times are validated before use.
