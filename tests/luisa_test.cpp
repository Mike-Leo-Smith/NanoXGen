#include "nanoxgen/generate.h"

#include <luisa/core/logging.h>
#include <luisa/core/stl/vector.h>
#include <luisa/dsl/syntax.h>
#include <luisa/runtime/buffer.h>
#include <luisa/runtime/context.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/stream.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace luisa;
using namespace luisa::compute;

namespace {

double milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
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
    std::cout << "{\"backend\":\"" << backend
              << "\",\"element_count\":" << count
              << ",\"compile_ms\":"
              << milliseconds(compile_end - compile_start)
              << ",\"upload_dispatch_download_ms\":"
              << milliseconds(dispatch_end - dispatch_start)
              << ",\"checksum\":\"0x" << std::hex << checksum
              << "\"}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
