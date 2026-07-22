#include "nanoxgen/asset.h"
#include "nanoxgen/luisa/generate.h"
#include "nanoxgen/luisa/xgen_classic_runtime.h"
#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <luisa/runtime/context.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/stream.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace luisa;
using namespace luisa::compute;

double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

std::uint32_t parse_u32(std::string_view text, const char *label,
                        bool allow_zero = false) {
    std::size_t consumed{};
    const unsigned long value = std::stoul(std::string{text}, &consumed);
    if (consumed != text.size() || (!allow_zero && value == 0u) ||
        value > 0xfffffffful) {
        throw std::invalid_argument(std::string{label} + " is invalid");
    }
    return static_cast<std::uint32_t>(value);
}

struct CaseSpec {
    std::string description;
    std::filesystem::path asset;
    std::uint32_t strands{};
    std::uint32_t cvs{};
};

CaseSpec parse_case(std::string_view value) {
    std::vector<std::string_view> fields;
    std::size_t begin{};
    for (;;) {
        const std::size_t separator = value.find(',', begin);
        fields.emplace_back(value.substr(begin, separator - begin));
        if (separator == std::string_view::npos) { break; }
        begin = separator + 1u;
    }
    if (fields.size() != 4u || fields[0].empty() || fields[1].empty()) {
        throw std::invalid_argument(
            "--case must be DESCRIPTION,ASSET.nxg,STRANDS,CVS");
    }
    CaseSpec result{};
    result.description = fields[0];
    result.asset = fields[1];
    result.strands = parse_u32(fields[2], "strand count");
    result.cvs = parse_u32(fields[3], "CV count");
    if (result.cvs < 2u) {
        throw std::invalid_argument("CV count must be at least two");
    }
    return result;
}

struct Options {
    std::filesystem::path runtime_directory;
    std::string backend;
    std::filesystem::path collection;
    std::uint32_t warmup{3u};
    std::uint32_t repeats{15u};
    std::vector<CaseSpec> cases;
};

Options parse_options(int argc, char **argv) {
    if (argc < 6) {
        throw std::invalid_argument(
            "usage: nanoxgen_luisa_classic_benchmark RUNTIME_DIR BACKEND "
            "COLLECTION.xgen --case DESCRIPTION,ASSET.nxg,STRANDS,CVS "
            "[--case ...] [--warmup N] [--repeats N]");
    }
    Options result{};
    result.runtime_directory = argv[1];
    result.backend = argv[2];
    result.collection = argv[3];
    for (int index = 4; index < argc; ++index) {
        const std::string argument{argv[index]};
        if (++index >= argc) {
            throw std::invalid_argument("missing value after " + argument);
        }
        const std::string_view value{argv[index]};
        if (argument == "--case") {
            result.cases.emplace_back(parse_case(value));
        } else if (argument == "--warmup") {
            result.warmup = parse_u32(value, "warmup count", true);
        } else if (argument == "--repeats") {
            result.repeats = parse_u32(value, "repeat count");
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (result.cases.empty()) {
        throw std::invalid_argument("at least one --case is required");
    }
    return result;
}

std::size_t checked_points(const CaseSpec &spec) {
    const std::uint64_t count =
        static_cast<std::uint64_t>(spec.strands) * spec.cvs;
    if (count > std::numeric_limits<std::size_t>::max() /
                    sizeof(luisa::float4)) {
        throw std::overflow_error("point allocation is too large");
    }
    return static_cast<std::size_t>(count);
}

struct RuntimeCase {
    CaseSpec spec;
    nanoxgen::Asset asset;
    nanoxgen::ClassicFloatRuntimePlan plan;
    nanoxgen::GenerationParams params;
    std::vector<nanoxgen::RootSample> roots;
    std::vector<std::uint32_t> root_runtime;
    std::vector<luisa::float3> surface_tangents;
    std::vector<luisa::float3> noise_domain_positions;
    ByteBuffer asset_buffer;
    ByteBuffer root_buffer;
    Buffer<std::uint32_t> root_runtime_buffer;
    Buffer<float> ptex_buffer;
    Buffer<luisa::float3> surface_tangent_buffer;
    Buffer<luisa::float3> noise_domain_buffer;
    Buffer<luisa::float4> a;
    Buffer<luisa::float4> b;
    Buffer<luisa::float4> states;
    Shader1D<ByteBuffer, ByteBuffer, Buffer<luisa::float4>> generate;
    Shader1D<Buffer<luisa::float4>, Buffer<luisa::float4>, ByteBuffer,
             Buffer<std::uint32_t>,
             Buffer<float>,
             Buffer<luisa::float4>> primitive;
    std::vector<Shader1D<Buffer<luisa::float4>, Buffer<luisa::float4>,
                         ByteBuffer, Buffer<std::uint32_t>,
                         Buffer<float>,
                         Buffer<luisa::float4>>> cuts;
    std::vector<Shader1D<Buffer<luisa::float4>, Buffer<luisa::float4>,
                         ByteBuffer, Buffer<std::uint32_t>,
                         Buffer<float>,
                         Buffer<luisa::float3>,
                         Buffer<luisa::float3>,
                         Buffer<luisa::float4>>> noises;
    Shader1D<Buffer<luisa::float4>, ByteBuffer, Buffer<std::uint32_t>,
             Buffer<float>,
             Buffer<luisa::float4>> width;
    bool final_is_a{};

    RuntimeCase(const CaseSpec &input,
                const nanoxgen::ClassicCollection &collection,
                Device &device)
        : spec{input}, asset{nanoxgen::load_asset(spec.asset)},
          plan{[&] {
              const auto *description = nanoxgen::find_classic_description(
                  collection, spec.description);
              if (!description) {
                  throw std::runtime_error(
                      "description not found: " + spec.description);
              }
              return nanoxgen::compile_xgen_classic_float_runtime_plan(
                  *description, collection.palette_attributes);
          }()} {
        params.strand_count = spec.strands;
        params.cvs_per_strand = spec.cvs;
        params.seed = 0x13579bdu;
        nanoxgen::PackedGeneratedCurves sampled =
            nanoxgen::generate_packed_cpu(asset, params);
        roots = std::move(sampled.roots);
        surface_tangents.reserve(roots.size());
        noise_domain_positions.reserve(roots.size());
        for (const nanoxgen::RootSample &root : roots) {
            const nanoxgen::Vec3 tangent = nanoxgen::root_surface_u(
                asset.view(), {}, root);
            surface_tangents.emplace_back(tangent.x, tangent.y, tangent.z);
            noise_domain_positions.emplace_back(
                root.position.x, root.position.y, root.position.z);
        }
        root_runtime.resize(roots.size() * 2u);
        for (std::size_t strand = 0u; strand < roots.size(); ++strand) {
            const nanoxgen::RootSample &root = roots[strand];
            root_runtime[strand * 2u] = static_cast<std::uint32_t>(strand);
            const std::array<double, 3u> prefix_arguments{
                static_cast<double>(root.uv.x),
                static_cast<double>(root.uv.y),
                static_cast<double>(nanoxgen::xgen_runtime_face_seed(
                    plan.description_id, plan.description_name,
                    root.surface_face_id))};
            root_runtime[strand * 2u + 1u] =
                nanoxgen::xgen_seexpr_hash_prefix(prefix_arguments);
        }
        asset_buffer = device.create_byte_buffer(asset.bytes().size());
        root_buffer = device.create_byte_buffer(
            roots.size() * sizeof(nanoxgen::RootSample));
        root_runtime_buffer = device.create_buffer<std::uint32_t>(
            root_runtime.size());
        ptex_buffer = device.create_buffer<float>(1u);
        surface_tangent_buffer = device.create_buffer<luisa::float3>(
            surface_tangents.size());
        noise_domain_buffer = device.create_buffer<luisa::float3>(
            noise_domain_positions.size());
        a = device.create_buffer<luisa::float4>(checked_points(spec));
        b = device.create_buffer<luisa::float4>(checked_points(spec));
        states = device.create_buffer<luisa::float4>(spec.strands);
        generate = device.compile(
            nanoxgen::luisa_backend::make_packed_generate_from_roots_kernel(
                asset, params));
        primitive = device.compile(
            nanoxgen::luisa_backend::make_classic_runtime_primitive_kernel(
                plan, spec.cvs));
        cuts.reserve(plan.cuts.size());
        for (const auto &cut : plan.cuts) {
            cuts.emplace_back(device.compile(
                nanoxgen::luisa_backend::make_classic_runtime_cut_kernel(
                    plan, cut, spec.cvs)));
        }
        noises.reserve(plan.noises.size());
        for (const auto &noise : plan.noises) {
            noises.emplace_back(device.compile(
                nanoxgen::luisa_backend::make_classic_runtime_noise_kernel(
                    plan, noise, spec.cvs)));
        }
        width = device.compile(
            nanoxgen::luisa_backend::make_classic_runtime_width_kernel(
                plan, spec.cvs));
        final_is_a = (plan.effects.size() % 2u) != 0u;
    }

    void upload(Stream &stream) {
        static constexpr std::array<float, 1u> empty_ptex{0.0f};
        stream << asset_buffer.copy_from(asset.bytes().data())
               << root_buffer.copy_from(roots.data())
               << root_runtime_buffer.copy_from(root_runtime.data())
               << ptex_buffer.copy_from(empty_ptex.data())
               << surface_tangent_buffer.copy_from(surface_tangents.data())
               << noise_domain_buffer.copy_from(noise_domain_positions.data());
    }

    void dispatch(Stream &stream) {
        stream << generate(asset_buffer, root_buffer, a).dispatch(spec.strands)
               << primitive(a, b, root_buffer, root_runtime_buffer,
                            ptex_buffer, states)
                      .dispatch(spec.strands);
        bool source_is_b = true;
        for (const nanoxgen::ClassicFloatEffect effect : plan.effects) {
            if (effect.type == nanoxgen::ClassicFloatEffectType::Noise) {
                auto &noise = noises.at(effect.module_index);
                if (source_is_b) {
                    stream << noise(b, a, root_buffer, root_runtime_buffer,
                                    ptex_buffer, surface_tangent_buffer,
                                    noise_domain_buffer, states)
                                  .dispatch(spec.strands);
                } else {
                    stream << noise(a, b, root_buffer, root_runtime_buffer,
                                    ptex_buffer, surface_tangent_buffer,
                                    noise_domain_buffer, states)
                                  .dispatch(spec.strands);
                }
            } else {
                auto &cut = cuts.at(effect.module_index);
                if (source_is_b) {
                    stream << cut(b, a, root_buffer, root_runtime_buffer,
                                  ptex_buffer, states)
                                  .dispatch(spec.strands);
                } else {
                    stream << cut(a, b, root_buffer, root_runtime_buffer,
                                  ptex_buffer, states)
                                  .dispatch(spec.strands);
                }
            }
            source_is_b = !source_is_b;
        }
        stream << width(final_is_a ? a : b, root_buffer,
                        root_runtime_buffer, ptex_buffer, states)
                      .dispatch(spec.strands);
    }

    [[nodiscard]] Buffer<luisa::float4> &final_points() noexcept {
        return final_is_a ? a : b;
    }
};

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const std::size_t index = std::min(
        values.size() - 1u,
        static_cast<std::size_t>(fraction * static_cast<double>(values.size())));
    return values[index];
}

std::uint64_t checksum(std::span<const luisa::float4> points,
                       std::uint64_t hash) {
    for (const luisa::float4 point : points) {
        for (const float value : {point.x, point.y, point.z, point.w}) {
            hash ^= std::bit_cast<std::uint32_t>(value);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

} // namespace

int main(int argc, char **argv) try {
    const Options options = parse_options(argc, argv);
    const Clock::time_point process_begin = Clock::now();
    Context context{options.runtime_directory.c_str()};
    Device device = context.create_device(options.backend.c_str());
    if (device.backend_name() != options.backend) {
        throw std::runtime_error("Luisa loaded an unexpected backend");
    }
    Stream stream = device.create_stream();
    const Clock::time_point device_end = Clock::now();
    const nanoxgen::ClassicCollection collection =
        nanoxgen::load_xgen_classic_collection(options.collection);
    std::vector<std::unique_ptr<RuntimeCase>> cases;
    cases.reserve(options.cases.size());
    for (const CaseSpec &spec : options.cases) {
        cases.emplace_back(
            std::make_unique<RuntimeCase>(spec, collection, device));
        cases.back()->upload(stream);
    }
    stream << synchronize();
    const Clock::time_point setup_end = Clock::now();

    const auto dispatch_all = [&] {
        for (auto &runtime : cases) { runtime->dispatch(stream); }
        stream << synchronize();
    };
    const Clock::time_point cold_begin = Clock::now();
    dispatch_all();
    const Clock::time_point cold_end = Clock::now();
    for (std::uint32_t repeat = 0u; repeat < options.warmup; ++repeat) {
        dispatch_all();
    }
    std::vector<double> samples;
    samples.reserve(options.repeats);
    for (std::uint32_t repeat = 0u; repeat < options.repeats; ++repeat) {
        const Clock::time_point begin = Clock::now();
        dispatch_all();
        samples.emplace_back(milliseconds(Clock::now() - begin));
    }

    const Clock::time_point checksum_begin = Clock::now();
    std::uint64_t output_checksum = 1469598103934665603ull;
    std::uint64_t strands{};
    std::uint64_t points{};
    for (auto &runtime : cases) {
        std::vector<luisa::float4> downloaded(checked_points(runtime->spec));
        stream << runtime->final_points().copy_to(luisa::span{downloaded})
               << synchronize();
        output_checksum = checksum(downloaded, output_checksum);
        strands += runtime->spec.strands;
        points += downloaded.size();
    }
    const Clock::time_point end = Clock::now();
    std::cout << std::setprecision(9)
              << "{\"backend\":\"" << options.backend
              << "\",\"descriptions\":" << cases.size()
              << ",\"strands\":" << strands
              << ",\"points\":" << points
              << ",\"device_create_ms\":"
              << milliseconds(device_end - process_begin)
              << ",\"asset_root_compile_upload_ms\":"
              << milliseconds(setup_end - device_end)
              << ",\"first_dispatch_ms\":"
              << milliseconds(cold_end - cold_begin)
              << ",\"warm_median_ms\":" << percentile(samples, 0.5)
              << ",\"warm_p90_ms\":" << percentile(samples, 0.9)
              << ",\"download_checksum_ms\":"
              << milliseconds(end - checksum_begin)
              << ",\"checksum\":" << output_checksum
              << ",\"handwritten_gpu_api\":false}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
