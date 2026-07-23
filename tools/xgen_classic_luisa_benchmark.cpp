#include "nanoxgen/curve_cache.h"
#include "nanoxgen/luisa/generate.h"
#include "nanoxgen/luisa/xgen_classic_runtime.h"
#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_alembic.h"
#include "nanoxgen/xgen_classic_clump.h"
#include "nanoxgen/xgen_classic_ptex.h"
#include "nanoxgen/xgen_classic_roots.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <luisa/runtime/context.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/stream.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace luisa;
using namespace luisa::compute;

double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::uint32_t parse_u32(std::string_view text, const char *label,
                        bool allow_zero = false) {
    std::size_t consumed{};
    const unsigned long value = std::stoul(std::string{text}, &consumed);
    if (consumed != text.size() || (!allow_zero && value == 0u) ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string{label} + " is invalid");
    }
    return static_cast<std::uint32_t>(value);
}

struct Options {
    std::filesystem::path runtime_directory;
    std::string backend;
    std::filesystem::path collection;
    std::filesystem::path archive;
    std::filesystem::path descriptions_root;
    std::string description;
    std::uint32_t warmup{3u};
    std::uint32_t repeats{11u};
    bool base_only{};
    std::uint32_t effect_count{std::numeric_limits<std::uint32_t>::max()};
    std::optional<std::filesystem::path> reference_nxc;
    std::optional<std::filesystem::path> output_nxc;
    bool fast_math{true};
    bool cpu_validation{true};
};

Options parse_options(int argc, char **argv) {
    if (argc < 7) {
        throw std::invalid_argument(
            "usage: nanoxgen_xgen_classic_luisa_benchmark RUNTIME_DIR "
            "BACKEND COLLECTION.xgen PATCHES.abc DESCRIPTIONS_ROOT "
            "DESCRIPTION [--warmup N] [--repeats N] [--base-only] "
            "[--effect-count N] [--strict-math] "
            "[--no-cpu-validation] "
            "[--reference-nxc FILE] [--output-nxc FILE]");
    }
    Options result{argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]};
    for (int index = 7; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--base-only") {
            result.base_only = true;
            continue;
        }
        if (argument == "--strict-math") {
            result.fast_math = false;
            continue;
        }
        if (argument == "--no-cpu-validation") {
            result.cpu_validation = false;
            continue;
        }
        if (argument != "--warmup" && argument != "--repeats" &&
            argument != "--effect-count" && argument != "--reference-nxc" &&
            argument != "--output-nxc") {
            throw std::invalid_argument(
                "unknown argument: " + std::string{argument});
        }
        if (++index >= argc) {
            throw std::invalid_argument(
                "missing value after " + std::string{argument});
        }
        if (argument == "--reference-nxc") {
            result.reference_nxc = argv[index];
        } else if (argument == "--output-nxc") {
            result.output_nxc = argv[index];
        } else if (argument == "--warmup") {
            result.warmup = parse_u32(argv[index], "warmup", true);
        } else if (argument == "--effect-count") {
            result.effect_count = parse_u32(argv[index], "effect count", true);
        } else {
            result.repeats = parse_u32(argv[index], "repeats");
        }
    }
    return result;
}

double percentile(std::vector<double> samples, double fraction) {
    std::sort(samples.begin(), samples.end());
    const std::size_t index = std::min(
        samples.size() - 1u,
        static_cast<std::size_t>(fraction * samples.size()));
    return samples[index];
}

std::uint64_t checksum(const nanoxgen::PackedGeneratedCurves &curves) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const nanoxgen::PackedCurvePoint point : curves.points) {
        for (const float value : {point.x, point.y, point.z, point.radius}) {
            hash ^= std::bit_cast<std::uint32_t>(value);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

nanoxgen::PackedGeneratedCurves compact_gpu_output(
    std::span<const luisa::float4> raw_points,
    std::span<const luisa::float4> states,
    const nanoxgen::ClassicRootPlan &root_plan,
    std::uint32_t cvs_per_strand) {
    if (states.size() != root_plan.roots.size() ||
        raw_points.size() != states.size() * cvs_per_strand) {
        throw std::runtime_error("Luisa output dimensions are invalid");
    }
    nanoxgen::PackedGeneratedCurves result{};
    result.cvs_per_strand = cvs_per_strand;
    result.points.reserve(raw_points.size());
    result.roots.reserve(states.size());
    result.root_uvs.reserve(states.size());
    result.point_counts.reserve(states.size());
    for (std::size_t strand = 0u; strand < states.size(); ++strand) {
        if (states[strand].x < 0.0f) { continue; }
        const auto begin = raw_points.begin() + strand * cvs_per_strand;
        for (std::uint32_t cv = 0u; cv < cvs_per_strand; ++cv) {
            const luisa::float4 point = begin[cv];
            result.points.push_back({point.x, point.y, point.z, point.w});
        }
        result.roots.push_back(root_plan.roots[strand]);
        result.root_uvs.push_back(root_plan.roots[strand].uv);
        result.point_counts.push_back(cvs_per_strand);
    }
    result.strand_count = static_cast<std::uint32_t>(result.roots.size());
    nanoxgen::add_xgen_classic_renderer_endpoints(result);
    return result;
}

struct ErrorStats {
    float position{};
    float radius{};
    std::uint64_t bit_mismatches{};
};

struct ClumpHostData {
    std::vector<luisa::float4> axes;
    std::vector<luisa::float4> frames;
    std::vector<std::uint32_t> runtime;
    std::vector<std::uint32_t> strand_guides;
    std::uint32_t guide_count{};
};

ErrorStats compare(const nanoxgen::PackedGeneratedCurves &a,
                   const nanoxgen::PackedGeneratedCurves &b) {
    if (a.strand_count != b.strand_count ||
        a.point_counts != b.point_counts || a.points.size() != b.points.size()) {
        throw std::runtime_error("Luisa/CPU Classic topology mismatch");
    }
    ErrorStats result{};
    for (std::size_t index = 0u; index < a.points.size(); ++index) {
        const auto &x = a.points[index];
        const auto &y = b.points[index];
        result.position = std::max({result.position,
            std::abs(x.x - y.x), std::abs(x.y - y.y), std::abs(x.z - y.z)});
        result.radius = std::max(result.radius,
                                 std::abs(x.radius - y.radius));
        for (const auto [lhs, rhs] : {std::pair{x.x, y.x},
                                      std::pair{x.y, y.y},
                                      std::pair{x.z, y.z},
                                      std::pair{x.radius, y.radius}}) {
            result.bit_mismatches +=
                std::bit_cast<std::uint32_t>(lhs) !=
                std::bit_cast<std::uint32_t>(rhs);
        }
    }
    return result;
}

ErrorStats compare_cache(const nanoxgen::PackedGeneratedCurves &generated,
                         const nanoxgen::CurveCache &reference) {
    const nanoxgen::CurveCacheView view = reference.view();
    if (view.header().strand_count != generated.strand_count ||
        view.header().point_count != generated.points.size() ||
        !view.face_ids() || !view.face_uvs() ||
        generated.roots.size() != generated.strand_count) {
        throw std::runtime_error("Luisa/renderer-reference topology mismatch");
    }
    using Identity =
        std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
    struct IndexedCurve {
        Identity identity;
        std::uint64_t point_offset{};
        std::uint32_t point_count{};
    };
    std::vector<IndexedCurve> generated_curves;
    std::vector<IndexedCurve> reference_curves;
    generated_curves.reserve(generated.strand_count);
    reference_curves.reserve(generated.strand_count);
    std::uint64_t generated_offset{};
    std::uint64_t reference_offset{};
    for (std::uint32_t strand = 0u; strand < generated.strand_count; ++strand) {
        const nanoxgen::RootSample root = generated.roots[strand];
        generated_curves.push_back({
            {root.surface_face_id, std::bit_cast<std::uint32_t>(root.uv.x),
             std::bit_cast<std::uint32_t>(root.uv.y)},
            generated_offset, generated.point_counts[strand]});
        const nanoxgen::Vec2 uv = view.face_uvs()[strand];
        reference_curves.push_back({
            {view.face_ids()[strand], std::bit_cast<std::uint32_t>(uv.x),
             std::bit_cast<std::uint32_t>(uv.y)},
            reference_offset, view.point_counts()[strand]});
        generated_offset += generated.point_counts[strand];
        reference_offset += view.point_counts()[strand];
    }
    const auto less = [](const IndexedCurve &a, const IndexedCurve &b) {
        return a.identity < b.identity;
    };
    std::sort(generated_curves.begin(), generated_curves.end(), less);
    std::sort(reference_curves.begin(), reference_curves.end(), less);
    ErrorStats result{};
    for (std::size_t strand = 0u; strand < generated_curves.size(); ++strand) {
        if ((strand != 0u &&
             (generated_curves[strand - 1u].identity ==
                  generated_curves[strand].identity ||
              reference_curves[strand - 1u].identity ==
                  reference_curves[strand].identity)) ||
            generated_curves[strand].identity !=
                reference_curves[strand].identity ||
            generated_curves[strand].point_count !=
                reference_curves[strand].point_count) {
            throw std::runtime_error(
                "Luisa/renderer-reference canonical identity mismatch");
        }
        for (std::uint32_t cv = 0u;
             cv < generated_curves[strand].point_count; ++cv) {
            const nanoxgen::PackedCurvePoint &a = generated.points[
                generated_curves[strand].point_offset + cv];
            const nanoxgen::PackedCurvePoint &b = view.points()[
                reference_curves[strand].point_offset + cv];
            result.position = std::max({
                result.position, std::abs(a.x - b.x),
                std::abs(a.y - b.y), std::abs(a.z - b.z)});
            result.radius = std::max(
                result.radius, std::abs(a.radius - b.radius));
            for (const auto [lhs, rhs] : {
                     std::pair{a.x, b.x}, std::pair{a.y, b.y},
                     std::pair{a.z, b.z}, std::pair{a.radius, b.radius}}) {
                result.bit_mismatches +=
                    std::bit_cast<std::uint32_t>(lhs) !=
                    std::bit_cast<std::uint32_t>(rhs);
            }
        }
    }
    return result;
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
    const nanoxgen::ClassicDescription *description =
        nanoxgen::find_classic_description(collection, options.description);
    if (description == nullptr) {
        throw std::runtime_error("Classic description was not found");
    }
    const nanoxgen::ClassicAlembicAssetInput imported =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            *description, options.archive);
    const nanoxgen::ClassicRootPlan root_plan =
        nanoxgen::build_xgen_classic_random_root_plan(
            *description, imported,
            options.descriptions_root / description->name);
    nanoxgen::ClassicFloatRuntimePlan runtime =
        nanoxgen::compile_xgen_classic_float_runtime_plan(
            *description, collection.palette_attributes);
    if (!runtime.lowering_complete()) {
        throw std::runtime_error(
            "description needs fallback: " + runtime.fallback_reasons.front());
    }
    if (root_plan.roots.empty() || root_plan.influence_offsets.empty()) {
        throw std::runtime_error("Classic root plan has no guide associations");
    }
    if (description->patches.empty()) {
        throw std::runtime_error("Classic PTEX runtime needs one patch");
    }
    const nanoxgen::ClassicRuntimeInputData runtime_inputs =
        nanoxgen::build_xgen_classic_runtime_input_data(
            runtime, options.descriptions_root / description->name,
            description->patches.front().name, root_plan);
    if (root_plan.primitive_ids.size() != root_plan.roots.size() ||
        root_plan.random_prefixes.size() != root_plan.roots.size() ||
        root_plan.surface_tangents.size() != root_plan.roots.size() ||
        root_plan.reference_positions.size() != root_plan.roots.size() ||
        root_plan.influence_offsets.size() != root_plan.roots.size() + 1u) {
        throw std::runtime_error("Classic root plan metadata is inconsistent");
    }
    const std::uint32_t cvs = runtime.fx_cv_count;
    std::vector<nanoxgen::ClassicClumpRuntimeData> clump_data;
    clump_data.reserve(runtime.clumps.size());
    for (std::size_t module = 0u; module < runtime.clumps.size(); ++module) {
        clump_data.push_back(
            nanoxgen::build_xgen_classic_clump_runtime_data(
                *description, imported,
                options.descriptions_root / description->name,
                root_plan, runtime, module, cvs));
    }
    if (options.effect_count < runtime.effects.size()) {
        runtime.effects.resize(options.effect_count);
    }
    const std::vector<nanoxgen::Vec3> rebuilt =
        nanoxgen::rebuild_xgen_classic_guides_for_device(imported.asset, cvs);
    std::vector<luisa::float3> rebuilt_gpu;
    rebuilt_gpu.reserve(rebuilt.size());
    for (const nanoxgen::Vec3 value : rebuilt) {
        rebuilt_gpu.emplace_back(value.x, value.y, value.z);
    }
    std::vector<luisa::float3> tangents;
    tangents.reserve(root_plan.surface_tangents.size());
    for (const nanoxgen::Vec3 value : root_plan.surface_tangents) {
        tangents.emplace_back(value.x, value.y, value.z);
    }
    std::vector<luisa::float3> noise_domain_positions;
    noise_domain_positions.reserve(root_plan.reference_positions.size());
    for (const nanoxgen::Vec3 value : root_plan.reference_positions) {
        noise_domain_positions.emplace_back(value.x, value.y, value.z);
    }
    std::vector<std::uint32_t> root_runtime(root_plan.roots.size() * 2u);
    for (std::size_t strand = 0u; strand < root_plan.roots.size(); ++strand) {
        root_runtime[strand * 2u] = root_plan.primitive_ids[strand];
        root_runtime[strand * 2u + 1u] = root_plan.random_prefixes[strand];
    }
    std::vector<ClumpHostData> clump_host;
    clump_host.reserve(clump_data.size());
    for (const nanoxgen::ClassicClumpRuntimeData &binding : clump_data) {
        ClumpHostData host{};
        host.guide_count = static_cast<std::uint32_t>(
            binding.guide_axes.size() / cvs);
        host.axes.reserve(binding.guide_axes.size());
        for (const nanoxgen::Vec3 value : binding.guide_axes) {
            host.axes.emplace_back(value.x, value.y, value.z, 0.0f);
        }
        host.frames.reserve(static_cast<std::size_t>(host.guide_count) * 3u);
        host.runtime.reserve(static_cast<std::size_t>(host.guide_count) * 2u);
        for (std::uint32_t guide = 0u; guide < host.guide_count; ++guide) {
            const nanoxgen::Vec3 normal = binding.guide_normals[guide];
            const nanoxgen::Vec3 tangent = binding.guide_tangents[guide];
            const nanoxgen::Vec2 uv = binding.guide_uvs[guide];
            host.frames.emplace_back(normal.x, normal.y, normal.z, uv.x);
            host.frames.emplace_back(tangent.x, tangent.y, tangent.z, uv.y);
            const nanoxgen::Vec3 domain_position =
                binding.guide_reference_positions.empty()
                ? binding.guide_axes[static_cast<std::size_t>(guide) * cvs]
                : binding.guide_reference_positions[guide];
            host.frames.emplace_back(
                domain_position.x, domain_position.y, domain_position.z, 0.0f);
            host.runtime.push_back(binding.guide_face_ids[guide]);
            host.runtime.push_back(binding.guide_random_prefixes[guide]);
        }
        host.strand_guides = binding.strand_guide_indices;
        clump_host.emplace_back(std::move(host));
    }
    const Clock::time_point native_prepare_end = Clock::now();

    const std::size_t strand_count = root_plan.roots.size();
    const std::size_t point_count = strand_count * cvs;
    ByteBuffer roots = device.create_byte_buffer(
        strand_count * sizeof(nanoxgen::RootSample));
    Buffer<std::uint32_t> offsets = device.create_buffer<std::uint32_t>(
        root_plan.influence_offsets.size());
    ByteBuffer influences = device.create_byte_buffer(
        root_plan.influences.size() * sizeof(nanoxgen::ClassicGuideInfluence));
    Buffer<luisa::float3> guides =
        device.create_buffer<luisa::float3>(rebuilt_gpu.size());
    Buffer<std::uint32_t> runtime_data =
        device.create_buffer<std::uint32_t>(root_runtime.size());
    std::vector<float> ptex_upload = runtime_inputs.values;
    if (ptex_upload.empty()) { ptex_upload.push_back(0.0f); }
    Buffer<float> ptex_buffer =
        device.create_buffer<float>(ptex_upload.size());
    Buffer<luisa::float3> tangent_buffer =
        device.create_buffer<luisa::float3>(tangents.size());
    Buffer<luisa::float3> noise_domain_buffer =
        device.create_buffer<luisa::float3>(noise_domain_positions.size());
    Buffer<luisa::float4> a = device.create_buffer<luisa::float4>(point_count);
    Buffer<luisa::float4> b = device.create_buffer<luisa::float4>(point_count);
    Buffer<luisa::float4> states =
        device.create_buffer<luisa::float4>(strand_count);
    std::vector<Buffer<luisa::float4>> clump_axes;
    std::vector<Buffer<luisa::float4>> clump_frames;
    std::vector<Buffer<std::uint32_t>> clump_runtime;
    std::vector<Buffer<std::uint32_t>> clump_strand_guides;
    clump_axes.reserve(clump_host.size());
    clump_frames.reserve(clump_host.size());
    clump_runtime.reserve(clump_host.size());
    clump_strand_guides.reserve(clump_host.size());
    for (const ClumpHostData &host : clump_host) {
        clump_axes.emplace_back(
            device.create_buffer<luisa::float4>(host.axes.size()));
        clump_frames.emplace_back(
            device.create_buffer<luisa::float4>(host.frames.size()));
        clump_runtime.emplace_back(
            device.create_buffer<std::uint32_t>(host.runtime.size()));
        clump_strand_guides.emplace_back(
            device.create_buffer<std::uint32_t>(host.strand_guides.size()));
    }

    ShaderOption shader_option{};
    shader_option.enable_cache = false;
    shader_option.enable_fast_math = options.fast_math;
    // HIP and Vulkan use independent external compiler invocations for these
    // kernels and Luisa's device compilation path is safe to enter in
    // parallel. Keep other backends sequential until they are validated: in
    // particular, the fallback backend currently has process-global compiler
    // state in the Luisa next branch.
    const bool parallel_jit =
        options.backend == "hip" || options.backend == "vk";
    const std::launch jit_launch = parallel_jit
        ? std::launch::async
        : std::launch::deferred;
    using NoiseShader = Shader1D<Buffer<luisa::float4>, Buffer<luisa::float4>,
        ByteBuffer, Buffer<std::uint32_t>, Buffer<float>,
        Buffer<luisa::float3>,
        Buffer<luisa::float3>,
        Buffer<luisa::float4>>;
    using CutShader = Shader1D<Buffer<luisa::float4>, Buffer<luisa::float4>,
        ByteBuffer, Buffer<std::uint32_t>, Buffer<float>,
        Buffer<luisa::float4>>;
    using ClumpShader = Shader1D<
        Buffer<luisa::float4>, Buffer<luisa::float4>, ByteBuffer,
        Buffer<std::uint32_t>, Buffer<float>, Buffer<luisa::float4>,
        Buffer<luisa::float4>,
        Buffer<luisa::float4>, Buffer<std::uint32_t>,
        Buffer<std::uint32_t>>;
    auto base_kernel =
        nanoxgen::luisa_backend::make_classic_base_generate_kernel(cvs);
    auto primitive_kernel =
        nanoxgen::luisa_backend::make_classic_runtime_primitive_kernel(
            runtime, cvs);
    auto width_kernel =
        nanoxgen::luisa_backend::make_classic_runtime_width_kernel(
            runtime, cvs);
    auto base_future = std::async(
        jit_launch,
        [&device, shader_option, kernel = std::move(base_kernel)]() mutable {
            return device.compile(kernel, shader_option);
        });
    auto primitive_future = std::async(
        jit_launch,
        [&device, shader_option,
         kernel = std::move(primitive_kernel)]() mutable {
            return device.compile(kernel, shader_option);
        });
    auto width_future = std::async(
        jit_launch,
        [&device, shader_option, kernel = std::move(width_kernel)]() mutable {
            return device.compile(kernel, shader_option);
        });
    std::vector<std::optional<NoiseShader>> noises(runtime.noises.size());
    std::vector<std::optional<CutShader>> cuts(runtime.cuts.size());
    std::vector<std::optional<ClumpShader>> clumps(runtime.clumps.size());
    std::vector<std::optional<std::future<NoiseShader>>> noise_futures(
        runtime.noises.size());
    std::vector<std::optional<std::future<CutShader>>> cut_futures(
        runtime.cuts.size());
    std::vector<std::optional<std::future<ClumpShader>>> clump_futures(
        runtime.clumps.size());
    for (const nanoxgen::ClassicFloatEffect effect : runtime.effects) {
        if (effect.type == nanoxgen::ClassicFloatEffectType::Noise) {
            auto &future = noise_futures.at(effect.module_index);
            if (!future) {
                auto kernel =
                    nanoxgen::luisa_backend::make_classic_runtime_noise_kernel(
                        runtime, runtime.noises[effect.module_index], cvs);
                future.emplace(std::async(
                    jit_launch,
                    [&device, shader_option,
                     kernel = std::move(kernel)]() mutable {
                        return device.compile(kernel, shader_option);
                    }));
            }
        } else if (effect.type == nanoxgen::ClassicFloatEffectType::Cut) {
            auto &future = cut_futures.at(effect.module_index);
            if (!future) {
                auto kernel =
                    nanoxgen::luisa_backend::make_classic_runtime_cut_kernel(
                        runtime, runtime.cuts[effect.module_index], cvs);
                future.emplace(std::async(
                    jit_launch,
                    [&device, shader_option,
                     kernel = std::move(kernel)]() mutable {
                        return device.compile(kernel, shader_option);
                    }));
            }
        } else {
            auto &future = clump_futures.at(effect.module_index);
            if (!future) {
                auto kernel =
                    nanoxgen::luisa_backend::make_classic_runtime_clump_kernel(
                        runtime, runtime.clumps[effect.module_index], cvs,
                        clump_host[effect.module_index].guide_count);
                future.emplace(std::async(
                    jit_launch,
                    [&device, shader_option,
                     kernel = std::move(kernel)]() mutable {
                        return device.compile(kernel, shader_option);
                    }));
            }
        }
    }
    auto base = base_future.get();
    auto primitive = primitive_future.get();
    auto width = width_future.get();
    for (std::size_t module = 0u; module < noise_futures.size(); ++module) {
        if (noise_futures[module]) {
            noises[module].emplace(noise_futures[module]->get());
        }
    }
    for (std::size_t module = 0u; module < cut_futures.size(); ++module) {
        if (cut_futures[module]) {
            cuts[module].emplace(cut_futures[module]->get());
        }
    }
    for (std::size_t module = 0u; module < clump_futures.size(); ++module) {
        if (clump_futures[module]) {
            clumps[module].emplace(clump_futures[module]->get());
        }
    }
    const Clock::time_point compile_end = Clock::now();

    stream << roots.copy_from(root_plan.roots.data())
           << offsets.copy_from(root_plan.influence_offsets.data())
           << influences.copy_from(root_plan.influences.data())
           << guides.copy_from(rebuilt_gpu.data())
           << runtime_data.copy_from(root_runtime.data())
           << ptex_buffer.copy_from(ptex_upload.data())
           << tangent_buffer.copy_from(tangents.data())
           << noise_domain_buffer.copy_from(noise_domain_positions.data());
    for (std::size_t module = 0u; module < clump_host.size(); ++module) {
        stream << clump_axes[module].copy_from(clump_host[module].axes.data())
               << clump_frames[module].copy_from(
                      clump_host[module].frames.data())
               << clump_runtime[module].copy_from(
                      clump_host[module].runtime.data())
               << clump_strand_guides[module].copy_from(
                      clump_host[module].strand_guides.data());
    }
    stream << synchronize();
    const Clock::time_point upload_end = Clock::now();

    bool final_is_a = (runtime.effects.size() % 2u) != 0u;
    const auto dispatch = [&] {
        stream << base(roots, offsets, influences, guides, a)
                      .dispatch(static_cast<std::uint32_t>(strand_count));
        if (options.base_only) { return; }
        stream << primitive(a, b, roots, runtime_data, ptex_buffer, states)
                      .dispatch(static_cast<std::uint32_t>(strand_count));
        bool source_is_b = true;
        for (const nanoxgen::ClassicFloatEffect effect : runtime.effects) {
            if (effect.type == nanoxgen::ClassicFloatEffectType::Clump) {
                auto &shader = clumps.at(effect.module_index).value();
                stream << (source_is_b
                    ? shader(b, a, roots, runtime_data, ptex_buffer, states,
                             clump_axes[effect.module_index],
                             clump_frames[effect.module_index],
                             clump_runtime[effect.module_index],
                             clump_strand_guides[effect.module_index])
                    : shader(a, b, roots, runtime_data, ptex_buffer, states,
                             clump_axes[effect.module_index],
                             clump_frames[effect.module_index],
                             clump_runtime[effect.module_index],
                             clump_strand_guides[effect.module_index]))
                    .dispatch(static_cast<std::uint32_t>(strand_count));
            } else if (effect.type == nanoxgen::ClassicFloatEffectType::Noise) {
                auto &shader = noises.at(effect.module_index).value();
                stream << (source_is_b
                    ? shader(b, a, roots, runtime_data, ptex_buffer,
                             tangent_buffer, noise_domain_buffer, states)
                    : shader(a, b, roots, runtime_data, ptex_buffer,
                             tangent_buffer, noise_domain_buffer, states))
                    .dispatch(static_cast<std::uint32_t>(strand_count));
            } else {
                auto &shader = cuts.at(effect.module_index).value();
                stream << (source_is_b
                    ? shader(b, a, roots, runtime_data, ptex_buffer, states)
                    : shader(a, b, roots, runtime_data, ptex_buffer, states))
                    .dispatch(static_cast<std::uint32_t>(strand_count));
            }
            source_is_b = !source_is_b;
        }
        stream << width(final_is_a ? a : b, roots, runtime_data,
                        ptex_buffer, states)
                      .dispatch(static_cast<std::uint32_t>(strand_count));
    };

    std::vector<luisa::float4> raw_points(point_count);
    std::vector<luisa::float4> raw_states(
        strand_count, luisa::make_float4(0.0f));
    const Clock::time_point first_begin = Clock::now();
    dispatch();
    stream << (options.base_only ? a : (final_is_a ? a : b))
                  .copy_to(luisa::span{raw_points});
    if (!options.base_only) {
        stream << states.copy_to(luisa::span{raw_states});
    }
    stream << synchronize();
    nanoxgen::PackedGeneratedCurves gpu = compact_gpu_output(
        raw_points, raw_states, root_plan, cvs);
    const Clock::time_point first_output_end = Clock::now();

    for (std::uint32_t i = 0u; i < options.warmup; ++i) {
        dispatch();
        stream << synchronize();
    }
    std::vector<double> warm_samples;
    warm_samples.reserve(options.repeats);
    for (std::uint32_t i = 0u; i < options.repeats; ++i) {
        const Clock::time_point begin = Clock::now();
        dispatch();
        stream << synchronize();
        warm_samples.push_back(milliseconds(begin, Clock::now()));
    }

    const Clock::time_point cpu_begin = Clock::now();
    std::optional<nanoxgen::PackedGeneratedCurves> cpu;
    std::optional<ErrorStats> error;
    if (options.cpu_validation) {
        cpu.emplace(nanoxgen::generate_xgen_classic_base_curves_cpu(
            imported.asset, root_plan, cvs));
        if (!options.base_only) {
            nanoxgen::apply_xgen_classic_float_runtime_plan_cpu(
                *cpu, runtime, 1.0f, root_plan.surface_tangents,
                root_plan.random_prefixes, root_plan.primitive_ids, clump_data,
                runtime_inputs.values, root_plan.reference_positions);
        }
        nanoxgen::add_xgen_classic_renderer_endpoints(*cpu);
        error = compare(gpu, *cpu);
    }
    const Clock::time_point cpu_end = Clock::now();
    std::optional<ErrorStats> reference_error;
    std::optional<ErrorStats> cpu_reference_error;
    if (options.reference_nxc) {
        const nanoxgen::CurveCache reference =
            nanoxgen::load_curve_cache(*options.reference_nxc);
        reference_error = compare_cache(gpu, reference);
        if (cpu) { cpu_reference_error = compare_cache(*cpu, reference); }
    }
    if (options.output_nxc) {
        std::vector<std::uint32_t> face_ids(gpu.strand_count);
        std::vector<nanoxgen::Vec2> face_uvs(gpu.strand_count);
        for (std::uint32_t strand = 0u; strand < gpu.strand_count; ++strand) {
            face_ids[strand] = gpu.roots[strand].surface_face_id;
            face_uvs[strand] = gpu.roots[strand].uv;
        }
        nanoxgen::save_curve_cache(
            nanoxgen::build_curve_cache(
                {gpu.point_counts, gpu.points, {}, {}, face_uvs, face_ids,
                 {}, {}}),
            *options.output_nxc);
    }
    constexpr float oracle_position_tolerance = 1.0e-3f;
    constexpr float oracle_radius_tolerance = 1.0e-7f;
    const bool oracle_within_tolerance = reference_error &&
        reference_error->position <= oracle_position_tolerance &&
        reference_error->radius <= oracle_radius_tolerance;

    std::cout << std::setprecision(9)
              << "{\"backend\":\"" << options.backend
              << "\",\"description\":\"" << description->name
              << "\",\"base_only\":"
              << (options.base_only ? "true" : "false")
              << ",\"effect_count\":" << runtime.effects.size()
              << ",\"parallel_jit\":"
              << (parallel_jit ? "true" : "false")
              << ",\"shader_cache\":false,\"fast_math\":"
              << (options.fast_math ? "true" : "false")
              << ",\"cpu_validation\":"
              << (options.cpu_validation ? "true" : "false")
              << ",\"includes_file_io\":true"
              << ",\"includes_autodesk_serialization\":false"
              << ",\"reference_comparison_order\":\"canonical-face-uv\""
              << ",\"input_roots\":" << strand_count
              << ",\"patch_culled\":" << root_plan.patch_culled_count
              << ",\"output_strands\":" << gpu.strand_count
              << ",\"output_points\":" << gpu.points.size()
              << ",\"device_create_ms\":"
              << milliseconds(process_begin, device_end)
              << ",\"native_parse_import_root_rebuild_ms\":"
              << milliseconds(device_end, native_prepare_end)
              << ",\"jit_compile_allocate_ms\":"
              << milliseconds(native_prepare_end, compile_end)
              << ",\"upload_ms\":" << milliseconds(compile_end, upload_end)
              << ",\"first_dispatch_download_pack_ms\":"
              << milliseconds(first_begin, first_output_end)
              << ",\"cold_end_to_end_ms\":"
              << milliseconds(process_begin, first_output_end)
              << ",\"warm_median_ms\":"
              << percentile(warm_samples, 0.5)
              << ",\"warm_p90_ms\":"
              << percentile(warm_samples, 0.9)
              << ",\"cpu_generation_only_ms\":"
              << (options.cpu_validation
                      ? milliseconds(cpu_begin, cpu_end) : -1.0)
              << ",\"max_position_error_vs_cpu\":"
              << (error ? error->position : -1.0f)
              << ",\"max_radius_error_vs_cpu\":"
              << (error ? error->radius : -1.0f)
              << ",\"bit_mismatches_vs_cpu\":"
              << (error
                      ? static_cast<long long>(error->bit_mismatches) : -1ll)
              << ",\"max_position_error_vs_reference\":"
              << (reference_error ? reference_error->position : -1.0f)
              << ",\"max_radius_error_vs_reference\":"
              << (reference_error ? reference_error->radius : -1.0f)
              << ",\"cpu_max_position_error_vs_reference\":"
              << (cpu_reference_error ? cpu_reference_error->position : -1.0f)
              << ",\"cpu_max_radius_error_vs_reference\":"
              << (cpu_reference_error ? cpu_reference_error->radius : -1.0f)
              << ",\"oracle_position_tolerance\":"
              << oracle_position_tolerance
              << ",\"oracle_radius_tolerance\":"
              << oracle_radius_tolerance
              << ",\"oracle_within_tolerance\":"
              << (oracle_within_tolerance ? "true" : "false")
              << ",\"checksum\":" << checksum(gpu)
              << ",\"fallback_count\":0"
              << ",\"handwritten_gpu_api\":false}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
