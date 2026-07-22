#include "nanoxgen/xgen_expression.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace nanoxgen;

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

void require_near(double actual, double expected, const char *message) {
    if (std::abs(actual - expected) > 2.0e-15) {
        throw std::runtime_error(
            std::string{message} + ": expected " + std::to_string(expected) +
            ", got " + std::to_string(actual));
    }
}

void require_fails(const std::string &source, const char *needle,
                   const XgenExpressionCompileOptions &options = {}) {
    try {
        (void)compile_xgen_scalar_expression(source, options);
    } catch (const std::exception &error) {
        if (std::string{error.what()}.find(needle) == std::string::npos) {
            throw std::runtime_error(
                "expected diagnostic '" + std::string{needle} + "', got '" +
                error.what() + "'");
        }
        return;
    }
    throw std::runtime_error("invalid expression was accepted");
}

void test_hash_and_seeds() {
    const std::array one{1.0};
    require_near(xgen_seexpr_hash(one), 0.40419923151009696,
                 "SeExpr hash calibration mismatch");
    require(xgen_string_seed("widthSplinePrimitive") == 83u,
            "XGen string seed mismatch");
    require(xgen_expression_seed("width", "SplinePrimitive") == 83u,
            "XGen expression seed mismatch");
    require_near(xgen_face_seed(0u, "rabbitShape", 7u),
                 0.82524268837302051, "XGen face seed mismatch");
    require_near(xgen_face_seed(0u, "namespace:rabbitShape", 7u),
                 0.82524268837302051, "XGen namespace stripping mismatch");
}

void test_compile_and_evaluate() {
    XgenExpressionCompileOptions options{};
    options.expression_name = "amount";
    options.object_type = "CutFXModule";
    const XgenExpressionProgram program = compile_xgen_scalar_expression(
        "$a=hash($foo+355) <= .3? rand(0.1,0.7):rand(0.2,0);\\n"
        "$a*$bar",
        options);
    require(program.inputs.size() == 2u && program.inputs[0] == "foo" &&
                program.inputs[1] == "bar",
            "expression input order mismatch");
    require(program.random_call_count == 2u, "rand site count mismatch");
    const std::array inputs{1.0, 2.0};
    require_near(evaluate_xgen_scalar_expression(
                     program, {inputs, 0.2, 0.3,
                               xgen_face_seed(0u, "rabbitShape", 7u)}),
                 0.1137663251985252,
                 "CPU expression did not match Autodesk oracle");

    options.expression_name = "noise";
    options.object_type = "ClumpingFXModule";
    const XgenExpressionProgram sequence = compile_xgen_scalar_expression(
        "$a=rand(0.1,0.8);$b=rand(1,3);$a+$b", options);
    require_near(evaluate_xgen_scalar_expression(
                     sequence, {{}, 0.2, 0.3,
                                xgen_face_seed(0u, "rabbitShape", 7u)}),
                 3.1409747049307857,
                 "rand site sequencing did not match Autodesk oracle");
}

void test_comments_operators_and_validation() {
    const XgenExpressionProgram program = compile_xgen_scalar_expression(
        "$percent=30;#0,100\\n"
        "$inside=hash($id)<=($percent/100);\\n"
        "$inside ? max(2,clamp($length,0,1)) : fit(.5,0,1,4,6)");
    require(program.inputs.size() == 2u, "external input discovery mismatch");
    std::array<double, 2u> inputs{};
    for (std::size_t i = 0u; i < program.inputs.size(); ++i) {
        inputs[i] = program.inputs[i] == "id" ? 1.0 : 0.25;
    }
    require_near(evaluate_xgen_scalar_expression(program, {inputs}), 5.0,
                 "conditional/local/function evaluation mismatch");

    require_fails("map('${DESC}/paintmaps/mask')", "PTEX binding");
    require_fails("noise($u)", "unsupported function");
}

void test_runtime_validation_and_limits() {
    const XgenExpressionProgram division = compile_xgen_scalar_expression("$a/0");
    const std::array input{1.0};
    try {
        (void)evaluate_xgen_scalar_expression(division, {input});
        throw std::runtime_error("non-finite expression result was accepted");
    } catch (const std::runtime_error &error) {
        require(std::string{error.what()}.find("non-finite") != std::string::npos,
                "wrong non-finite diagnostic");
    }
    const std::array nan_input{std::numeric_limits<double>::quiet_NaN()};
    try {
        (void)evaluate_xgen_scalar_expression(division, {nan_input});
        throw std::runtime_error("non-finite expression input was accepted");
    } catch (const std::runtime_error &error) {
        require(std::string{error.what()}.find("non-finite") != std::string::npos,
                "wrong non-finite input diagnostic");
    }

    XgenExpressionCompileOptions options{};
    options.max_source_bytes = 3u;
    require_fails("1234", "source byte limit", options);
    options = {};
    options.max_instructions = 2u;
    require_fails("1+2", "instruction limit", options);
}

void test_float_runtime_ir() {
    XgenExpressionCompileOptions options{};
    options.expression_name = "amount";
    options.object_type = "CutFXModule";
    const XgenFloatExpressionProgram program =
        make_xgen_float_expression_program(compile_xgen_scalar_expression(
            "$a=hash($id+355) <= .3? rand(0.1,0.7):rand(0.2,0);"
            "$a*$cLength",
            options));
    static_assert(sizeof(decltype(XgenFloatScalarInstruction::immediate)) ==
                  sizeof(float));
    const std::array inputs{1.0f, 2.0f};
    const float value = evaluate_xgen_scalar_expression_float(
        program, {inputs, 0.2f, 0.3f,
                  xgen_runtime_face_seed(0u, "rabbitShape", 7u)});
    require(std::isfinite(value), "float runtime expression is non-finite");

    const float namespaced =
        xgen_runtime_face_seed(0u, "namespace:rabbitShape", 7u);
    require(namespaced == xgen_runtime_face_seed(0u, "rabbitShape", 7u),
            "float runtime namespace stripping mismatch");

    XgenExpressionProgram overflow = compile_xgen_scalar_expression("1");
    overflow.instructions[0].immediate =
        std::numeric_limits<double>::max();
    try {
        (void)make_xgen_float_expression_program(overflow);
        throw std::runtime_error("non-representable float immediate was accepted");
    } catch (const std::runtime_error &error) {
        require(std::string{error.what()}.find("represented as float") !=
                    std::string::npos,
                "wrong float immediate diagnostic");
    }
}

void test_ramp_ui() {
    struct Case {
        const char *source;
        double t;
        double expected;
    };
    const std::array cases{
        Case{"rampUI(0,0,0:0.5,1,0:1,0,0)", 0.25, 0.0},
        Case{"rampUI(0,0,1:0.5,1,1:1,0,1)", 0.25, 0.5},
        Case{"rampUI(0,0,2:0.5,1,2:1,0,2)", 0.125, 0.15625},
        Case{"rampUI(0,0,3:0.5,1,3:1,0,3)", 0.25, 0.5625},
        Case{"rampUI(0,-1,1:1,2,1)", 0.25, 0.0},
        Case{"rampUI(0,-1,1:1,2,1)", 0.75, 1.0},
    };
    for (const Case &test : cases) {
        const XgenExpressionProgram strict =
            compile_xgen_scalar_expression(test.source);
        require_near(evaluate_xgen_scalar_expression(
                         strict, {{}, 0.0, 0.0, 0.0, test.t}),
                     test.expected, "rampUI did not match Autodesk oracle");
        const XgenFloatExpressionProgram runtime =
            make_xgen_float_expression_program(strict);
        const float value = evaluate_xgen_scalar_expression_float(
            runtime, {{}, 0.0f, 0.0f, 0.0f, static_cast<float>(test.t)});
        require(std::abs(value - static_cast<float>(test.expected)) <= 1.0e-6f,
                "float rampUI evaluation mismatch");
    }

    require_fails("rampUI(0,0,1:0,1,1)", "strictly increasing");
    require_fails("rampUI(-.1,0,1:1,1,1)", "positions must be");
    require_fails("rampUI(0,0,4:1,1,4)", "integer in [0, 3]");
    XgenExpressionCompileOptions options{};
    options.max_ramp_points = 1u;
    require_fails("rampUI(0,0,1:1,1,1)", "ramp point limit", options);
}

} // namespace

int main() try {
    test_hash_and_seeds();
    test_compile_and_evaluate();
    test_comments_operators_and_validation();
    test_runtime_validation_and_limits();
    test_float_runtime_ir();
    test_ramp_ui();
    std::cout << "XGen scalar expression tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
