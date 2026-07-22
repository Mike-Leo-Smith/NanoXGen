#include "nanoxgen/generate.h"
#include "nanoxgen/luisa/xgen_classic_runtime.h"
#include "nanoxgen/luisa/xgen_expression.h"
#include "nanoxgen/xgen_classic_runtime.h"
#include "nanoxgen/xgen_expression.h"

#if defined(NANOXGEN_TEST_LUISA_HIP_INTEROP)
#include "nanoxgen/hip.h"
#include <hip/hip_runtime_api.h>
#endif

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

#if defined(NANOXGEN_TEST_LUISA_HIP_INTEROP)

void check_hip(hipError_t error, const char *operation) {
    if (error != hipSuccess) {
        throw std::runtime_error(
            std::string{operation} + ": " + hipGetErrorString(error));
    }
}

class HipAllocation {
public:
    explicit HipAllocation(std::size_t bytes) : _bytes{bytes} {
        if (bytes != 0u) {
            check_hip(hipMalloc(&_data, bytes), "hipMalloc");
        }
    }
    HipAllocation(const HipAllocation &) = delete;
    HipAllocation &operator=(const HipAllocation &) = delete;
    ~HipAllocation() {
        if (_data != nullptr) { (void)hipFree(_data); }
    }
    [[nodiscard]] void *data() const noexcept { return _data; }
    [[nodiscard]] std::size_t bytes() const noexcept { return _bytes; }
private:
    void *_data{};
    std::size_t _bytes{};
};

nanoxgen::Asset make_interop_asset() {
    nanoxgen::AssetBuildInput input{};
    input.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                       {0.0f, 0.0f, 1.0f}, {1.0f, 0.1f, 1.0f}};
    input.normals.assign(input.positions.size(), {0.0f, 1.0f, 0.0f});
    input.texcoords = {{0.0f, 0.0f}, {1.0f, 0.0f},
                       {0.0f, 1.0f}, {1.0f, 1.0f}};
    input.triangles = {{0u, 1u, 2u}, {1u, 3u, 2u}};
    nanoxgen::GuideInput a{};
    a.triangle_index = 0u;
    a.barycentric = {0.2f, 0.3f};
    a.root_normal = {0.0f, 1.0f, 0.0f};
    a.support_radius = 10.0f;
    a.cvs = {{0.2f, 0.0f, 0.3f}, {0.18f, 0.3f, 0.33f},
             {0.25f, 0.7f, 0.38f}, {0.35f, 1.1f, 0.45f}};
    nanoxgen::GuideInput b{};
    b.triangle_index = 1u;
    b.barycentric = {0.35f, 0.25f};
    b.root_normal = {0.0f, 1.0f, 0.0f};
    b.support_radius = 10.0f;
    b.cvs = {{0.75f, 0.05f, 0.6f}, {0.82f, 0.4f, 0.57f},
             {0.78f, 0.78f, 0.68f}, {0.68f, 1.2f, 0.76f}};
    input.guides = {std::move(a), std::move(b)};
    return nanoxgen::build_asset(input);
}

nanoxgen::ClassicFloatRuntimePlan make_interop_plan() {
    nanoxgen::ClassicDescription description{};
    description.name = "luisaHipInterop";
    description.attributes.push_back({"descriptionId", "7", 1u});
    description.objects.push_back({"SplinePrimitive", {
        {"length", "1.25", 1u},
        {"width", "hash($id+17)*0.02+0.01", 1u},
        {"taper", "0.5", 1u},
        {"taperStart", "0.25", 1u},
        {"widthRamp", "rampUI(0,1,1:1,0.5,1)", 1u},
        {"fxCVCount", "8", 1u}}, 1u});
    description.objects.push_back({"CutFXModule", {
        {"active", "true", 1u}, {"name", "cut", 1u},
        {"amount", "0.25*$cLength", 1u},
        {"rebuildType", "1", 1u}}, 1u});
    auto plan = nanoxgen::compile_xgen_classic_float_runtime_plan(description);
    if (!plan.lowering_complete() || plan.cuts.size() != 1u) {
        throw std::runtime_error("HIP interop fixture did not lower completely");
    }
    return plan;
}

float test_hip_external_buffer_pipeline(Device &device, Stream &stream) {
    constexpr std::uint32_t strand_count = 1024u;
    constexpr std::uint32_t cvs_per_strand = 8u;
    constexpr float radius_scale = 1.0f;
    const nanoxgen::Asset asset = make_interop_asset();
    const nanoxgen::ClassicFloatRuntimePlan plan = make_interop_plan();
    nanoxgen::GenerationParams params{};
    params.strand_count = strand_count;
    params.cvs_per_strand = cvs_per_strand;
    params.seed = 0x1234abcdu;
    params.root_width = 0.03f;
    params.tip_width = 0.002f;
    params.noise_amplitude = 0.0f;
    nanoxgen::PackedGeneratedCurves reference =
        nanoxgen::generate_packed_cpu(asset, params, radius_scale, {1u, 128u});
    nanoxgen::apply_xgen_classic_float_runtime_plan_cpu(
        reference, plan, radius_scale);

    HipAllocation device_asset{asset.bytes().size()};
    HipAllocation points_a{reference.points.size() * sizeof(nanoxgen::PackedCurvePoint)};
    HipAllocation points_b{reference.points.size() * sizeof(nanoxgen::PackedCurvePoint)};
    HipAllocation roots{reference.roots.size() * sizeof(nanoxgen::RootSample)};
    HipAllocation root_uvs{reference.root_uvs.size() * sizeof(nanoxgen::Vec2)};
    HipAllocation point_counts{reference.point_counts.size() * sizeof(std::uint32_t)};
    check_hip(hipMemcpy(device_asset.data(), asset.bytes().data(),
                        asset.bytes().size(), hipMemcpyHostToDevice),
              "upload interop asset");
    const auto asset_descriptor = nanoxgen::make_device_asset_descriptor(
        asset, device_asset.data(), device_asset.bytes());
    const nanoxgen::DevicePackedCurveOutputDescriptor output{
        {static_cast<nanoxgen::PackedCurvePoint *>(points_a.data()),
         static_cast<nanoxgen::RootSample *>(roots.data()),
         static_cast<nanoxgen::Vec2 *>(root_uvs.data()), radius_scale,
         static_cast<std::uint32_t *>(point_counts.data())},
        reference.points.size(), reference.roots.size(),
        reference.root_uvs.size(), reference.point_counts.size()};
    check_hip(nanoxgen::launch_generate_packed_hip(
                  asset_descriptor, {}, params, output, {128u}),
              "launch native HIP packed generation");
    check_hip(hipDeviceSynchronize(), "native HIP packed generation sync");

    {
        auto a = device.import_external_buffer<luisa::float4>(
            points_a.data(), reference.points.size());
        auto b = device.import_external_buffer<luisa::float4>(
            points_b.data(), reference.points.size());
        auto root_bytes = device.import_external_byte_buffer(
            roots.data(), roots.bytes());
        auto states = device.create_buffer<luisa::float4>(strand_count);
        auto primitive = device.compile(
            nanoxgen::luisa_backend::make_classic_runtime_primitive_kernel(
                plan, cvs_per_strand, radius_scale));
        auto cut = device.compile(
            nanoxgen::luisa_backend::make_classic_runtime_cut_kernel(
                plan, plan.cuts.front(), cvs_per_strand));
        auto width = device.compile(
            nanoxgen::luisa_backend::make_classic_runtime_width_kernel(
                plan, cvs_per_strand, radius_scale));
        stream << primitive(a, b, root_bytes, states).dispatch(strand_count)
               << cut(b, a, root_bytes, states).dispatch(strand_count)
               << width(a, root_bytes, states).dispatch(strand_count)
               << synchronize();
    }

    std::vector<nanoxgen::PackedCurvePoint> actual(reference.points.size());
    check_hip(hipMemcpy(actual.data(), points_a.data(), points_a.bytes(),
                        hipMemcpyDeviceToHost),
              "download Luisa HIP packed points");
    float max_error = 0.0f;
    for (std::size_t i = 0u; i < actual.size(); ++i) {
        max_error = std::max({max_error,
            std::abs(actual[i].x - reference.points[i].x),
            std::abs(actual[i].y - reference.points[i].y),
            std::abs(actual[i].z - reference.points[i].z),
            std::abs(actual[i].radius - reference.points[i].radius)});
    }
    if (max_error > 1.0e-5f) {
        throw std::runtime_error(
            "native HIP -> Luisa external-buffer pipeline mismatch (max=" +
            std::to_string(max_error) + ")");
    }
    return max_error;
}

#endif

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
    const nanoxgen::ClassicFloatRuntimeExpression classic_expression{
        "CutFXModule", "Cut1", "amount", expression_program};
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
        nanoxgen::ClassicFloatRuntimeContext runtime_context{};
        runtime_context.id = i;
        runtime_context.u = expression_contexts[i];
        runtime_context.v = expression_contexts[i + expression_count];
        runtime_context.face_seed =
            expression_contexts[i + expression_count * 2u];
        runtime_context.t = expression_contexts[i + expression_count * 3u];
        for (std::size_t input_index = 0u;
             input_index < expression_program.inputs.size(); ++input_index) {
            if (expression_program.inputs[input_index] == "cLength") {
                runtime_context.c_length =
                    expression_inputs[i + input_index * expression_count];
            }
        }
        expression_reference[i] =
            nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                classic_expression, runtime_context);
    }
    Buffer<float> expression_input_buffer =
        device.create_buffer<float>(expression_inputs.size());
    Buffer<float> expression_context_buffer =
        device.create_buffer<float>(expression_contexts.size());
    Buffer<float> expression_output_buffer =
        device.create_buffer<float>(expression_results.size());
    std::size_t c_length_input = 0u;
    for (; c_length_input < expression_program.inputs.size(); ++c_length_input) {
        if (expression_program.inputs[c_length_input] == "cLength") { break; }
    }
    if (c_length_input == expression_program.inputs.size()) {
        throw std::runtime_error("Classic JIT fixture has no cLength input");
    }
    Kernel1D expression_kernel =
        [&](BufferFloat expression_input, BufferFloat expression_context,
            BufferFloat output) noexcept {
            set_block_size(128u, 1u, 1u);
            UInt index = dispatch_id().x;
            output.write(
                index,
                nanoxgen::luisa_backend::lower_classic_runtime_expression(
                    classic_expression,
                    {index,
                     expression_context.read(index),
                     expression_context.read(index + expression_count),
                     expression_context.read(index + expression_count * 2u),
                     expression_input.read(
                         index + expression_count *
                                     static_cast<std::uint32_t>(c_length_input)),
                     0.0f,
                     expression_context.read(index + expression_count * 3u)}));
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

#if defined(NANOXGEN_TEST_LUISA_HIP_INTEROP)
    const float hip_interop_max_absolute_error =
        test_hip_external_buffer_pipeline(device, stream);
#endif

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
#if defined(NANOXGEN_TEST_LUISA_HIP_INTEROP)
              << ",\"hip_external_buffer_points\":" << (1024u * 8u)
              << ",\"hip_external_buffer_max_absolute_error\":"
              << hip_interop_max_absolute_error
#endif
              << ",\"checksum\":\"0x" << std::hex << checksum
              << "\"}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
