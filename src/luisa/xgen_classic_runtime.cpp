#include "nanoxgen/luisa/xgen_classic_runtime.h"

#include "nanoxgen/luisa/xgen_expression.h"

#include <luisa/core/stl/vector.h>

#include <stdexcept>

namespace nanoxgen::luisa_backend {

using namespace luisa;
using namespace luisa::compute;

Expr<float> lower_classic_runtime_expression(
    const ClassicFloatRuntimeExpression &expression,
    const ClassicFloatRuntimeLuisaContext &context) {
    vector<Expr<float>> inputs;
    inputs.reserve(expression.program.inputs.size());
    for (const std::string &input : expression.program.inputs) {
        if (input == "id") {
            inputs.emplace_back(cast<float>(context.id));
        } else if (input == "cLength") {
            inputs.emplace_back(context.c_length);
        } else if (input == "cWidth") {
            inputs.emplace_back(context.c_width);
        } else {
            throw std::runtime_error(
                "Classic Luisa runtime variable is not bound: $" + input);
        }
    }
    return lower_expression(expression.program, inputs, context.u, context.v,
                            context.face_seed, context.t);
}

} // namespace nanoxgen::luisa_backend
