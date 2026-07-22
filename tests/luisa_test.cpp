#include "nanoxgen/generate.h"
#include "nanoxgen/luisa/xgen_expression.h"
#include "nanoxgen/xgen_expression.h"

#include <luisa/core/logging.h>
#include <luisa/core/stl/vector.h>
#include <luisa/dsl/syntax.h>
#include <luisa/runtime/buffer.h>
#include <luisa/runtime/context.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/stream.h>

#include <chrono>
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace luisa;
using namespace luisa::compute;

namespace {

float milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<float, std::milli>(duration).count();
}

} // namespace

int main(int argc, char **argv) try {
    if (argc != 3) {
        std::cerr << "usage: nanoxgen_luisa_tests <runtime-dir> <backend>\n";
        return 2;
    }
    const std::string runtime_dir = argv[1];
    const std::string backend = argv[2];
    Context context{runtime_dir.c_str()};
    Device device = context.create_device(backend.c_str());
    if (device.backend_name() != backend) {
        throw std::runtime_error("LuisaCompute loaded an unexpected backend");
    }

    constexpr std::uint32_t count = 65536u;
    std::vector<std::uint32_t> input(count);
    std::vector<std::uint32_t> hashes(count);
    std::vector<float> widths(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        input[index] = index * 17u + 11u;
    }

    Buffer<std::uint32_t> input_buffer =
        device.create_buffer<std::uint32_t>(count);
    Buffer<std::uint32_t> hash_buffer =
        device.create_buffer<std::uint32_t>(count);
    Buffer<float> width_buffer = device.create_buffer<float>(count);
    Kernel1D evaluate = [](BufferUInt input_values, BufferUInt output_hashes,
                           BufferFloat output_widths) noexcept {
        set_block_size(128u, 1u, 1u);
        UInt index = dispatch_id().x;
        UInt value = input_values.read(index);
        value = value ^ (value >> 16u);
        value = value * 0x7feb352du;
        value = value ^ (value >> 15u);
        value = value * 0x846ca68bu;
        value = value ^ (value >> 16u);
        output_hashes.write(index, value);
        Float unit = cast<float>(value >> 8u) * (1.0f / 16777216.0f);
        output_widths.write(index, unit * 0.01f + 0.01f);
    };

    const auto compile_start = std::chrono::steady_clock::now();
    auto shader = device.compile(evaluate);
    const auto compile_end = std::chrono::steady_clock::now();
    Stream stream = device.create_stream();
    const auto dispatch_start = std::chrono::steady_clock::now();
    stream << input_buffer.copy_from(luisa::span{input})
           << shader(input_buffer, hash_buffer, width_buffer).dispatch(count)
           << hash_buffer.copy_to(luisa::span{hashes})
           << width_buffer.copy_to(luisa::span{widths})
           << synchronize();
    const auto dispatch_end = std::chrono::steady_clock::now();

    nanoxgen::XgenExpressionCompileOptions expression_options{};
    expression_options.expression_name = "amount";
    expression_options.object_type = "CutFXModule";
    const nanoxgen::XgenFloatExpressionProgram expression_program =
        nanoxgen::make_xgen_float_expression_program(
            nanoxgen::compile_xgen_scalar_expression(
                "$a=hash($id+355) <= .3? rand(0.1,0.7):rand(0.2,0);"
                "$a*$cLength*rampUI(0,0,3:0.5,1,3:1,0,3)",
                expression_options));
    constexpr std::uint32_t expression_count = 4096u;
    std::vector<float> expression_inputs(
        expression_count * expression_program.inputs.size());
    std::vector<float> expression_contexts(expression_count * 4u);
    std::vector<float> expression_results(expression_count);
    std::vector<float> expression_reference(expression_count);
    for (std::uint32_t i = 0u; i < expression_count; ++i) {
        for (std::size_t input_index = 0u;
             input_index < expression_program.inputs.size(); ++input_index) {
            expression_inputs[i + input_index * expression_count] =
                expression_program.inputs[input_index] == "id"
                    ? static_cast<float>(i)
                    : 0.5f + static_cast<float>(i % 17u) * 0.03125f;
        }
        expression_contexts[i] = static_cast<float>(i % 101u) / 101.0f;
        expression_contexts[i + expression_count] =
            static_cast<float>((i * 7u) % 103u) / 103.0f;
        expression_contexts[i + expression_count * 2u] =
            nanoxgen::xgen_runtime_face_seed(
                2u, "rabbitShape", i % 152u);
        expression_contexts[i + expression_count * 3u] =
            static_cast<float>(i % 257u) / 256.0f;
        std::vector<float> input_values(expression_program.inputs.size());
        for (std::size_t input_index = 0u;
             input_index < expression_program.inputs.size(); ++input_index) {
            input_values[input_index] =
                expression_inputs[i + input_index * expression_count];
        }
        expression_reference[i] = nanoxgen::evaluate_xgen_scalar_expression_float(
            expression_program,
            {input_values, expression_contexts[i],
             expression_contexts[i + expression_count],
             expression_contexts[i + expression_count * 2u],
             expression_contexts[i + expression_count * 3u]});
    }
    Buffer<float> expression_input_buffer =
        device.create_buffer<float>(expression_inputs.size());
    Buffer<float> expression_context_buffer =
        device.create_buffer<float>(expression_contexts.size());
    Buffer<float> expression_output_buffer =
        device.create_buffer<float>(expression_results.size());
    Kernel1D expression_kernel =
        [&](BufferFloat expression_input, BufferFloat expression_context,
            BufferFloat output) noexcept {
            set_block_size(128u, 1u, 1u);
            UInt index = dispatch_id().x;
            output.write(index, nanoxgen::luisa_backend::lower_expression(
                expression_program, index, expression_count, expression_input,
                expression_context));
        };
    const auto expression_compile_start = std::chrono::steady_clock::now();
    auto expression_shader = device.compile(expression_kernel);
    const auto expression_compile_end = std::chrono::steady_clock::now();
    const auto expression_dispatch_start = std::chrono::steady_clock::now();
    stream << expression_input_buffer.copy_from(luisa::span{expression_inputs})
           << expression_context_buffer.copy_from(luisa::span{expression_contexts})
           << expression_shader(expression_input_buffer, expression_context_buffer,
                                expression_output_buffer)
                  .dispatch(expression_count)
           << expression_output_buffer.copy_to(luisa::span{expression_results})
           << synchronize();
    const auto expression_dispatch_end = std::chrono::steady_clock::now();

    std::uint64_t checksum = 1469598103934665603ull;
    for (std::uint32_t index = 0u; index < count; ++index) {
        const std::uint32_t expected_hash = nanoxgen::hash32(input[index]);
        const float expected_width =
            static_cast<float>(expected_hash >> 8u) *
                (1.0f / 16777216.0f) * 0.01f +
            0.01f;
        if (hashes[index] != expected_hash ||
            std::abs(widths[index] - expected_width) > 1.0e-7f) {
            throw std::runtime_error(
                "LuisaCompute CPU/GPU deterministic expression mismatch");
        }
        checksum ^= hashes[index];
        checksum *= 1099511628211ull;
    }
    std::size_t expression_bit_mismatches = 0u;
    std::uint32_t expression_max_ulp = 0u;
    float expression_max_absolute_error = 0.0f;
    for (std::uint32_t index = 0u; index < expression_count; ++index) {
        const std::uint32_t actual_bits =
            std::bit_cast<std::uint32_t>(expression_results[index]);
        const std::uint32_t expected_bits =
            std::bit_cast<std::uint32_t>(expression_reference[index]);
        if (actual_bits != expected_bits) {
            ++expression_bit_mismatches;
            const std::uint32_t ulp = actual_bits > expected_bits
                                          ? actual_bits - expected_bits
                                          : expected_bits - actual_bits;
            expression_max_ulp = std::max(expression_max_ulp, ulp);
            expression_max_absolute_error = std::max(
                expression_max_absolute_error,
                std::abs(expression_results[index] - expression_reference[index]));
        }
    }
    if (expression_max_absolute_error > 1.2e-7f) {
        throw std::runtime_error(
            "LuisaCompute float expression IR exceeded the absolute error bound (" +
            std::to_string(expression_max_absolute_error) + ")");
    }
    std::cout << "{\"backend\":\"" << backend
              << "\",\"element_count\":" << count
              << ",\"compile_ms\":"
              << milliseconds(compile_end - compile_start)
              << ",\"upload_dispatch_download_ms\":"
              << milliseconds(dispatch_end - dispatch_start)
              << ",\"xgen_expression_count\":" << expression_count
              << ",\"xgen_expression_semantics\":\"nanoxgen-fast-float\""
              << ",\"xgen_expression_compile_ms\":"
              << milliseconds(expression_compile_end - expression_compile_start)
              << ",\"xgen_expression_dispatch_ms\":"
              << milliseconds(expression_dispatch_end - expression_dispatch_start)
              << ",\"xgen_expression_bit_mismatches\":"
              << expression_bit_mismatches
              << ",\"xgen_expression_max_ulp\":" << expression_max_ulp
              << ",\"xgen_expression_max_absolute_error\":"
              << expression_max_absolute_error
              << ",\"checksum\":\"0x" << std::hex << checksum
              << "\"}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
