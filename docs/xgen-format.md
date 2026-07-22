# Interactive Grooming `.xgen` BLOB format

NanoXGen implements the version-1 binary `XgSplineData` container emitted from
Maya Interactive Grooming `outRenderData`. This format shares the `.xgen`
extension with classic XGen text descriptions, but the two formats are not the
same. The parser identifies the binary form by its file magic and never needs
Maya, XGen, or an Autodesk library.

All integer fields are unsigned 64-bit little-endian values. The file starts
with:

| Offset | Field |
|---:|---|
| 0 | magic `0x8099cead` |
| 8 | UTF-8 metadata JSON byte count |
| 16 | metadata JSON bytes |

The JSON `Header` declares `Type: XgSplineData`, file/group version 1, group
count, compression level, and storage flags. It is followed by groups in strict
index order. Each group has six 64-bit fields:

| Field | Meaning |
|---|---|
| magic | `0x041e065f` |
| stored size | 32-byte remaining header plus stored payload bytes |
| group index | zero-based, in file order |
| array count | records in the expanded payload |
| raw data bytes | sum of array data, excluding array headers |
| flags | preserved by generic round-trips |

Official binary fixtures set `GroupDeflate=true`; the payload is one zlib
stream. Its expanded size is `raw data bytes + array count * 16`. Each expanded
array record is `{uint64 typeTag, uint64 byteSize, byte data[byteSize]}`.
Metadata array IDs use `(groupIndex << 32) | arrayIndex`.

The renderer-relevant v1 tags verified so far are:

| Tag | Element |
|---:|---|
| `0x43f60844` | uint32 vector data |
| `0x000197ef` | uint32 scalar/index data |
| `0x05d0225c` | float32 |
| `0xdb53a6f4` | two float32 values |
| `0xdb53a713` | three float32 values |

`Items` references primitive ranges, positions, patch UVs, surface frames,
face UV/ID, frozen values, widths, and directions. `PrimitiveInfos` contains at
least offset and CV count per curve. NanoXGen materializes `Positions`,
`PatchUVs`, `FaceUV`, `FaceId`, and `WIDTH_CV`, then reconstructs the public
varying curve texcoord exactly as `(0, CV/(CVCount-1))`. The default full
materializer canonicalizes curves by the bit patterns of
`faceId + faceUV + patchUV`, matching the motion/cache identity contract. A
source-order renderer-minimal path skips that sort for one-shot static work.

The generic `XGenDocument` retains the original metadata string, unknown type
tags, unknown arrays, group flags, and compression settings. A parse/serialize
round-trip therefore does not discard data it cannot interpret.
`process_xgen_document` edits only referenced Positions/WIDTH arrays and keeps
the rest. `build_xgen_document` creates a compact evaluated BLOB with all 11
Item channels needed by the official loader; a reference mesh is optional for
already-evaluated curves.

## Tools

```bash
nanoxgen_xgen_inspect input.xgen
nanoxgen_xgen_inspect input.xgen --round-trip output.xgen
nanoxgen_xgen_process input.xgen output.xgen --length-scale 0.8 --width-scale 1.2
nanoxgen_xgen_process input.xgen minimal.xgen --rebuild --group-bytes 8388608
nanoxgen_xgen_cache input.xgen output.nxc
nanoxgen_xgen_cache --renderer-minimal --source-order input.xgen output.nxc
nanoxgen_xgen_read_benchmark input.xgen
```

The default CMake targets above link only NanoXGen, Threads, and zlib. The
optional Autodesk calibration targets generate fixtures and compare canonical
hashes/caches; they are not called by the parser, processor, writer, generator,
or converter.

## Scope and rejection policy

The binary API currently accepts version-1 group streams. It rejects malformed
sizes, unordered/duplicate groups, invalid JSON, bad zlib data, unknown file or
group versions, non-finite renderer values, inconsistent primitive ranges, and
out-of-range array references. `GroupBase64=true` belongs to XGen's separate
ASCII stream path and is rejected by this binary API instead of being guessed.
Classic XGen descriptions, XPD/XUV/XGC files, and future `XgSplineData`
versions are not parsed by this binary API. The package API inventories those
formats and their references without claiming to evaluate them.
