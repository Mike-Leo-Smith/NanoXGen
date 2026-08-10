#pragma once

#include "nanoxgen/context.h"
#include "nanoxgen/types.h"

#include <span>
#include <string_view>

namespace nanoxgen {

// Evaluate a typed SeExpr 1.x color expression for each reference position.
// The implementation is embedded and does not load an Autodesk/Maya runtime.
// rand() is rejected because XGen's expression/patch seed state is not part of
// plain SeExpr; accepting it would produce deterministic but incompatible data.
void evaluate_xgen_color_expression(
    std::string_view source,
    std::span<const Vec3> reference_positions,
    std::span<Vec3> values,
    NanoXGenContext *context = nullptr);

} // namespace nanoxgen
