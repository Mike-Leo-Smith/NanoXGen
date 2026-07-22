#include "nanoxgen/asset.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

namespace nanoxgen {
namespace {

constexpr std::size_t kSectionAlignment = 16u;

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

template<typename T>
std::uint64_t append_array(std::vector<std::byte> &blob, std::span<T> values) {
    if (values.empty()) { return 0u; }
    const std::size_t offset = align_up(blob.size(), std::max(kSectionAlignment, alignof(T)));
    blob.resize(offset + values.size_bytes());
    std::memcpy(blob.data() + offset, values.data(), values.size_bytes());
    return offset;
}

std::uint64_t fnv1a(std::span<const std::byte> bytes) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::byte b : bytes) {
        hash ^= static_cast<std::uint8_t>(b);
        hash *= 1099511628211ull;
    }
    return hash;
}

float triangle_area(Vec3 a, Vec3 b, Vec3 c) {
    return 0.5f * std::sqrt(length_squared(cross(b - a, c - a)));
}

std::vector<AliasEntry> make_alias_table(const AssetBuildInput &input) {
    const std::size_t n = input.triangles.size();
    std::vector<double> scaled(n);
    double area_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const UInt3 t = input.triangles[i];
        scaled[i] = triangle_area(input.positions[t.x], input.positions[t.y], input.positions[t.z]);
        area_sum += scaled[i];
    }
    if (!(area_sum > 0.0)) { throw std::invalid_argument("mesh has zero total area"); }
    for (double &p : scaled) { p = p * static_cast<double>(n) / area_sum; }

    std::vector<std::size_t> small;
    std::vector<std::size_t> large;
    for (std::size_t i = 0; i < n; ++i) { (scaled[i] < 1.0 ? small : large).push_back(i); }
    std::vector<AliasEntry> table(n);
    while (!small.empty() && !large.empty()) {
        const std::size_t s = small.back(); small.pop_back();
        const std::size_t l = large.back(); large.pop_back();
        table[s] = {static_cast<float>(scaled[s]), static_cast<std::uint32_t>(l)};
        scaled[l] = scaled[l] + scaled[s] - 1.0;
        (scaled[l] < 1.0 ? small : large).push_back(l);
    }
    for (const std::size_t i : large) { table[i] = {1.0f, static_cast<std::uint32_t>(i)}; }
    for (const std::size_t i : small) { table[i] = {1.0f, static_cast<std::uint32_t>(i)}; }
    return table;
}

std::vector<Vec3> make_normals(const AssetBuildInput &input) {
    if (input.normals.size() == input.positions.size()) { return input.normals; }
    std::vector<Vec3> normals(input.positions.size());
    for (const UInt3 t : input.triangles) {
        const Vec3 n = cross(input.positions[t.y] - input.positions[t.x], input.positions[t.z] - input.positions[t.x]);
        normals[t.x] = normals[t.x] + n;
        normals[t.y] = normals[t.y] + n;
        normals[t.z] = normals[t.z] + n;
    }
    for (Vec3 &n : normals) { n = normalize(n); }
    return normals;
}

float automatic_support_radius(const std::vector<GuideInput> &guides, std::size_t index) {
    if (guides[index].support_radius > 0.0f) { return guides[index].support_radius; }
    const Vec3 root = guides[index].cvs.front();
    float nearest = std::numeric_limits<float>::max();
    for (std::size_t j = 0; j < guides.size(); ++j) {
        if (j == index || guides[j].cvs.empty()) { continue; }
        nearest = std::min(nearest, std::sqrt(length_squared(root - guides[j].cvs.front())));
    }
    return std::isfinite(nearest) ? std::max(nearest * 1.75f, 1.0e-4f) : 1.0f;
}

void check_input(const AssetBuildInput &input) {
    if (input.positions.empty()) { throw std::invalid_argument("asset needs vertices"); }
    if (input.triangles.empty()) { throw std::invalid_argument("asset needs triangles"); }
    if (input.guides.empty()) { throw std::invalid_argument("asset needs at least one guide"); }
    if (!input.normals.empty() && input.normals.size() != input.positions.size()) {
        throw std::invalid_argument("normal count must match vertex count");
    }
    if (!input.texcoords.empty() && input.texcoords.size() != input.positions.size()) {
        throw std::invalid_argument("texcoord count must match vertex count");
    }
    for (const UInt3 t : input.triangles) {
        if (t.x >= input.positions.size() || t.y >= input.positions.size() || t.z >= input.positions.size()) {
            throw std::invalid_argument("triangle index out of bounds");
        }
    }
    for (const GuideInput &g : input.guides) {
        if (g.cvs.size() < 2u || g.cvs.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::invalid_argument("each guide needs 2..65535 CVs");
        }
    }
}

template<typename T>
bool section_fits(const AssetHeader &h, std::uint64_t offset, std::uint64_t count) {
    if (count == 0u) { return offset == 0u; }
    if (offset < sizeof(AssetHeader) || offset > h.byte_size) { return false; }
    const std::uint64_t size = count * sizeof(T);
    return size <= h.byte_size - offset;
}

template<typename Function>
void parallel_for_strands(
    std::uint32_t strand_count,
    const CpuGenerationOptions &options,
    Function &&function) {
    if (options.strands_per_work_block == 0u) {
        throw std::invalid_argument("CPU work block must contain at least one strand");
    }
    const std::uint32_t logical_block_count = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(strand_count) +
         options.strands_per_work_block - 1u) / options.strands_per_work_block);
    std::uint32_t worker_count = options.worker_count;
    if (worker_count == 0u) {
        worker_count = std::max(1u, std::thread::hardware_concurrency());
        const std::uint32_t useful_workers = (logical_block_count + 3u) / 4u;
        worker_count = std::min(worker_count, std::max(1u, useful_workers));
    }
    worker_count = std::min(worker_count, logical_block_count);

    std::atomic<std::uint32_t> next_block{0u};
    const auto worker = [&] {
        for (;;) {
            const std::uint32_t block = next_block.fetch_add(1u, std::memory_order_relaxed);
            if (block >= logical_block_count) { return; }
            const std::uint64_t first =
                static_cast<std::uint64_t>(block) * options.strands_per_work_block;
            const std::uint32_t end = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                first + options.strands_per_work_block, strand_count));
            for (std::uint32_t strand = static_cast<std::uint32_t>(first);
                 strand < end; ++strand) {
                function(strand);
            }
        }
    };
    if (worker_count == 1u) {
        worker();
        return;
    }
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::uint32_t i = 0u; i < worker_count; ++i) { workers.emplace_back(worker); }
    for (auto &thread : workers) { thread.join(); }
}

DeviceDeformedGeometryView validate_generation_input(
    const Asset &asset,
    const GenerationParams &params,
    const DeformedGeometryView &deformed) {
    const std::string error = validate_asset(asset.bytes());
    if (!error.empty()) { throw std::invalid_argument("invalid asset: " + error); }
    if (params.strand_count == 0u || params.cvs_per_strand < 2u) {
        throw std::invalid_argument("generation needs strands and at least two CVs");
    }
    const DeviceAssetView asset_view = asset.view();
    const AssetHeader &header = asset_view.header();
    if ((!deformed.positions.empty() && deformed.positions.size() != header.vertex_count) ||
        (!deformed.normals.empty() && deformed.normals.size() != header.vertex_count) ||
        (!deformed.guide_cvs.empty() && deformed.guide_cvs.size() != header.guide_cv_count)) {
        throw std::invalid_argument("deformed geometry array size does not match the asset");
    }
    return {
        deformed.positions.empty() ? nullptr : deformed.positions.data(),
        deformed.normals.empty() ? nullptr : deformed.normals.data(),
        deformed.guide_cvs.empty() ? nullptr : deformed.guide_cvs.data(),
    };
}

} // namespace

Asset build_asset(const AssetBuildInput &input) {
    check_input(input);
    const std::vector<Vec3> normals = make_normals(input);
    const std::vector<AliasEntry> alias_table = make_alias_table(input);

    std::vector<GuideRecord> guides;
    std::vector<Vec3> guide_cvs;
    guides.reserve(input.guides.size());
    for (std::size_t i = 0; i < input.guides.size(); ++i) {
        const GuideInput &source = input.guides[i];
        GuideRecord record{};
        record.first_cv = static_cast<std::uint32_t>(guide_cvs.size());
        record.cv_count = static_cast<std::uint16_t>(source.cvs.size());
        record.triangle_index = source.triangle_index;
        record.support_radius = automatic_support_radius(input.guides, i);
        record.barycentric = source.barycentric;
        record.root_uv = source.root_uv;
        record.root_position = source.cvs.front();
        record.root_normal = normalize(source.root_normal);
        guides.push_back(record);
        guide_cvs.insert(guide_cvs.end(), source.cvs.begin(), source.cvs.end());
    }

    std::vector<std::uint32_t> triangle_guides(input.triangles.size() * kGuideStencilSize, kInvalidIndex);
    for (std::size_t ti = 0; ti < input.triangles.size(); ++ti) {
        const UInt3 t = input.triangles[ti];
        const Vec3 centroid = (input.positions[t.x] + input.positions[t.y] + input.positions[t.z]) / 3.0f;
        const Vec3 face_normal = normalize(cross(input.positions[t.y] - input.positions[t.x],
                                                  input.positions[t.z] - input.positions[t.x]));
        std::vector<std::pair<float, std::uint32_t>> candidates;
        candidates.reserve(guides.size());
        for (std::uint32_t gi = 0; gi < guides.size(); ++gi) {
            const float normal_penalty = dot(face_normal, guides[gi].root_normal) < -0.25f ? 1.0e20f : 0.0f;
            candidates.emplace_back(length_squared(centroid - guides[gi].root_position) + normal_penalty, gi);
        }
        const std::size_t count = std::min<std::size_t>(kGuideStencilSize, candidates.size());
        std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end());
        for (std::size_t j = 0; j < count; ++j) {
            triangle_guides[ti * kGuideStencilSize + j] = candidates[j].second;
        }
    }

    std::vector<std::byte> blob(sizeof(AssetHeader));
    AssetHeader header{};
    header.flags = HasNormals | (input.texcoords.empty() ? 0u : HasTexcoords);
    header.vertex_count = static_cast<std::uint32_t>(input.positions.size());
    header.triangle_count = static_cast<std::uint32_t>(input.triangles.size());
    header.guide_count = static_cast<std::uint32_t>(guides.size());
    header.guide_cv_count = static_cast<std::uint32_t>(guide_cvs.size());
    header.positions_offset = append_array(blob, std::span{input.positions});
    header.normals_offset = append_array(blob, std::span{normals});
    header.texcoords_offset = append_array(blob, std::span{input.texcoords});
    header.triangles_offset = append_array(blob, std::span{input.triangles});
    header.alias_table_offset = append_array(blob, std::span{alias_table});
    header.guides_offset = append_array(blob, std::span{guides});
    header.guide_cvs_offset = append_array(blob, std::span{guide_cvs});
    header.triangle_guides_offset = append_array(blob, std::span{triangle_guides});
    header.byte_size = blob.size();
    std::memcpy(blob.data(), &header, sizeof(header));
    header.content_hash = fnv1a(std::span{blob}.subspan(sizeof(AssetHeader)));
    std::memcpy(blob.data(), &header, sizeof(header));
    return Asset{std::move(blob)};
}

std::string validate_asset(std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(AssetHeader)) { return "blob is smaller than AssetHeader"; }
    AssetHeader h{};
    std::memcpy(&h, bytes.data(), sizeof(h));
    if (h.magic != kMagic) { return "bad NanoXGen magic"; }
    if (h.version_major != kVersionMajor) { return "unsupported major version"; }
    if (h.byte_size != bytes.size()) { return "header byte_size does not match file size"; }
    if (h.triangle_count == 0u || h.vertex_count == 0u || h.guide_count == 0u) { return "required section is empty"; }
    if (h.guide_stencil_size != kGuideStencilSize) { return "unsupported guide stencil size"; }
    if (!section_fits<Vec3>(h, h.positions_offset, h.vertex_count) ||
        !section_fits<Vec3>(h, h.normals_offset, h.vertex_count) ||
        !section_fits<Vec2>(h, h.texcoords_offset, (h.flags & HasTexcoords) ? h.vertex_count : 0u) ||
        !section_fits<UInt3>(h, h.triangles_offset, h.triangle_count) ||
        !section_fits<AliasEntry>(h, h.alias_table_offset, h.triangle_count) ||
        !section_fits<GuideRecord>(h, h.guides_offset, h.guide_count) ||
        !section_fits<Vec3>(h, h.guide_cvs_offset, h.guide_cv_count) ||
        !section_fits<std::uint32_t>(h, h.triangle_guides_offset,
                                     static_cast<std::uint64_t>(h.triangle_count) * h.guide_stencil_size)) {
        return "one or more sections are out of bounds";
    }
    if (fnv1a(bytes.subspan(sizeof(AssetHeader))) != h.content_hash) { return "content hash mismatch"; }
    const DeviceAssetView view{bytes.data()};
    for (std::uint32_t i = 0; i < h.triangle_count; ++i) {
        const UInt3 t = view.triangles()[i];
        if (t.x >= h.vertex_count || t.y >= h.vertex_count || t.z >= h.vertex_count) {
            return "triangle index out of bounds";
        }
    }
    for (std::uint32_t i = 0; i < h.guide_count; ++i) {
        const GuideRecord &g = view.guides()[i];
        if (g.cv_count < 2u || g.first_cv > h.guide_cv_count ||
            g.cv_count > h.guide_cv_count - g.first_cv) { return "guide CV range out of bounds"; }
    }
    return {};
}

void save_asset(const Asset &asset, const std::filesystem::path &path) {
    const std::string error = validate_asset(asset.bytes());
    if (!error.empty()) { throw std::runtime_error("refusing to save invalid asset: " + error); }
    std::ofstream stream(path, std::ios::binary);
    if (!stream) { throw std::runtime_error("failed to open asset for writing: " + path.string()); }
    stream.write(reinterpret_cast<const char *>(asset.bytes().data()),
                 static_cast<std::streamsize>(asset.bytes().size()));
    if (!stream) { throw std::runtime_error("failed while writing asset: " + path.string()); }
}

Asset load_asset(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) { throw std::runtime_error("failed to open asset: " + path.string()); }
    const auto size = stream.tellg();
    if (size < 0) { throw std::runtime_error("failed to query asset size"); }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), size);
    if (!stream) { throw std::runtime_error("failed to read asset: " + path.string()); }
    const std::string error = validate_asset(bytes);
    if (!error.empty()) { throw std::runtime_error("invalid NanoXGen asset: " + error); }
    return Asset{std::move(bytes)};
}

GeneratedCurves generate_cpu(
    const Asset &asset, const GenerationParams &params, const CpuGenerationOptions &options) {
    return generate_deformed_cpu(asset, params, DeformedGeometryView{}, options);
}

GeneratedCurves generate_deformed_cpu(
    const Asset &asset,
    const GenerationParams &params,
    const DeformedGeometryView &deformed,
    const CpuGenerationOptions &options) {
    const DeviceDeformedGeometryView device_deformed =
        validate_generation_input(asset, params, deformed);
    const DeviceAssetView asset_view = asset.view();
    GeneratedCurves curves{};
    curves.strand_count = params.strand_count;
    curves.cvs_per_strand = params.cvs_per_strand;
    const std::size_t point_count = static_cast<std::size_t>(params.strand_count) * params.cvs_per_strand;
    curves.points.resize(point_count);
    curves.widths.resize(point_count);
    curves.roots.resize(params.strand_count);
    const GeneratedOutputView output{curves.points.data(), curves.widths.data(), curves.roots.data()};

    parallel_for_strands(params.strand_count, options, [&](std::uint32_t strand) {
        generate_strand(asset_view, params, strand, output, device_deformed);
    });
    return curves;
}

GeneratedCurves generate_cpu(const Asset &asset, const GenerationParams &params) {
    return generate_cpu(asset, params, CpuGenerationOptions{});
}

PackedGeneratedCurves generate_packed_cpu(
    const Asset &asset,
    const GenerationParams &params,
    float radius_scale,
    const CpuGenerationOptions &options) {
    return generate_packed_deformed_cpu(
        asset, params, DeformedGeometryView{}, radius_scale, options);
}

PackedGeneratedCurves generate_packed_deformed_cpu(
    const Asset &asset,
    const GenerationParams &params,
    const DeformedGeometryView &deformed,
    float radius_scale,
    const CpuGenerationOptions &options) {
    if (!std::isfinite(radius_scale) || radius_scale < 0.0f) {
        throw std::invalid_argument("curve radius scale must be finite and non-negative");
    }
    const DeviceDeformedGeometryView device_deformed =
        validate_generation_input(asset, params, deformed);
    PackedGeneratedCurves curves{};
    curves.strand_count = params.strand_count;
    curves.cvs_per_strand = params.cvs_per_strand;
    const std::size_t point_count =
        static_cast<std::size_t>(params.strand_count) * params.cvs_per_strand;
    curves.points.resize(point_count);
    curves.roots.resize(params.strand_count);
    curves.root_uvs.resize(params.strand_count);
    const DevicePackedCurveOutputView output{
        curves.points.data(), curves.roots.data(), curves.root_uvs.data(), radius_scale};
    const DeviceAssetView asset_view = asset.view();
    parallel_for_strands(params.strand_count, options, [&](std::uint32_t strand) {
        generate_packed_strand(asset_view, params, strand, output, device_deformed);
    });
    return curves;
}

GeneratedCurves generate_linear_modifier_reference_cpu(
    std::span<const LinearCurveSeed> seeds,
    const LinearModifierReferenceParams &params,
    const CpuGenerationOptions &options) {
    if (seeds.empty() || params.cvs_per_strand < 2u) {
        throw std::invalid_argument("linear generation needs seeds and at least two CVs");
    }
    if (seeds.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("too many linear curve seeds");
    }
    if (options.strands_per_work_block == 0u) {
        throw std::invalid_argument("CPU work block must contain at least one strand");
    }
    if (!std::isfinite(params.length_scale) || params.length_scale < 0.0f ||
        !std::isfinite(params.width_taper) || params.width_taper < 0.0f ||
        params.width_taper > 1.0f || !std::isfinite(params.width_taper_start) ||
        params.width_taper_start < 0.0f || params.width_taper_start > 1.0f) {
        throw std::invalid_argument("invalid linear generation parameters");
    }

    GeneratedCurves curves{};
    curves.strand_count = static_cast<std::uint32_t>(seeds.size());
    curves.cvs_per_strand = params.cvs_per_strand;
    const std::size_t point_count = seeds.size() * params.cvs_per_strand;
    curves.points.resize(point_count);
    curves.widths.resize(point_count);
    curves.roots.resize(seeds.size());

    const std::uint32_t logical_block_count = static_cast<std::uint32_t>(
        (seeds.size() + options.strands_per_work_block - 1u) /
        options.strands_per_work_block);
    std::uint32_t worker_count = options.worker_count;
    if (worker_count == 0u) {
        worker_count = std::max(1u, std::thread::hardware_concurrency());
        const std::uint32_t useful_workers = (logical_block_count + 3u) / 4u;
        worker_count = std::min(worker_count, std::max(1u, useful_workers));
    }
    worker_count = std::min(worker_count, logical_block_count);

    std::atomic<std::uint32_t> next_block{0u};
    const auto worker = [&] {
        for (;;) {
            const std::uint32_t block = next_block.fetch_add(1u, std::memory_order_relaxed);
            if (block >= logical_block_count) { return; }
            const std::size_t first = static_cast<std::size_t>(block) * options.strands_per_work_block;
            const std::size_t end = std::min(first + options.strands_per_work_block, seeds.size());
            for (std::size_t strand = first; strand < end; ++strand) {
                const LinearCurveSeed &seed = seeds[strand];
                const Vec3 direction = seed.tip - seed.root;
                curves.roots[strand] = {
                    seed.root, normalize(direction), seed.root_uv, kInvalidIndex, {}};
                for (std::uint32_t cv = 0u; cv < params.cvs_per_strand; ++cv) {
                    const float t = static_cast<float>(cv) /
                        static_cast<float>(params.cvs_per_strand - 1u);
                    const std::size_t index = strand * params.cvs_per_strand + cv;
                    curves.points[index] = seed.root + direction * (params.length_scale * t);
                    const float taper_t = params.width_taper_start < 1.0f
                        ? std::clamp((t - params.width_taper_start) /
                            (1.0f - params.width_taper_start), 0.0f, 1.0f)
                        : (t >= 1.0f ? 1.0f : 0.0f);
                    curves.widths[index] = seed.root_width *
                        (1.0f - params.width_taper * taper_t);
                }
            }
        }
    };

    if (worker_count == 1u) {
        worker();
    } else {
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::uint32_t i = 0u; i < worker_count; ++i) { workers.emplace_back(worker); }
        for (auto &thread : workers) { thread.join(); }
    }
    return curves;
}

GeneratedCurves generate_linear_cpu(
    std::span<const LinearCurveSeed> seeds,
    const LinearGenerationParams &params,
    const CpuGenerationOptions &options) {
    return generate_linear_modifier_reference_cpu(seeds, params, options);
}

void write_curves_obj(const GeneratedCurves &curves, const std::filesystem::path &path) {
    std::ofstream stream(path);
    if (!stream) { throw std::runtime_error("failed to open OBJ: " + path.string()); }
    stream << "# NanoXGen generated curve polyline preview\n";
    for (const Vec3 p : curves.points) { stream << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n'; }
    for (std::uint32_t strand = 0; strand < curves.strand_count; ++strand) {
        stream << 'l';
        for (std::uint32_t cv = 0; cv < curves.cvs_per_strand; ++cv) {
            stream << ' ' << (static_cast<std::uint64_t>(strand) * curves.cvs_per_strand + cv + 1u);
        }
        stream << '\n';
    }
}

} // namespace nanoxgen
