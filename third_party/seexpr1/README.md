# Embedded SeExpr 1.x

This directory contains the expression runtime from Disney SeExpr 1.0.1,
commit `01c0ee4fd0c67e07ac82ccf8847485e5d34a9431`, plus parser sources generated
with winflexbison 2.5.25.

NanoXGen uses it privately for Classic XGen typed `custom_color_*`
expressions. It is statically linked and has no Autodesk or Maya runtime
dependency. The source is covered by
`LICENSES/SeExpr-BSD-3-Clause.txt`.

The only portability change to the upstream sources removes legacy MSVC
definitions of `log2`, `uint32_t`, and `UINT32_MAX`; modern toolchains
provide them.
