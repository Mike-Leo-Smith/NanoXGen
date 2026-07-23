#include "nanoxgen/xgen_classic_clump.h"

#include "nanoxgen/xgen_classic_ptex.h"
#include "nanoxgen/xgen_ptex.h"
#include "nanoxgen/xpd.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nanoxgen {
namespace {

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error("Classic ClumpingFX: " + message);
}

const ClassicObject &find_module(
    const ClassicDescription &description, std::string_view name) {
    const ClassicObject *result = nullptr;
    for (const ClassicObject &object : description.objects) {
        if (object.type != "ClumpingFXModule") { continue; }
        const ClassicAttribute *object_name = find_classic_attribute(
            object.attributes, "name");
        if (!object_name || object_name->value != name) { continue; }
        if (result) { fail("duplicate module name " + std::string{name}); }
        result = &object;
    }
    if (!result) { fail("module was not found: " + std::string{name}); }
    return *result;
}

std::string replace_all(
    std::string value, std::string_view needle, std::string_view replacement) {
    for (std::size_t offset = 0u;
         (offset = value.find(needle, offset)) != std::string::npos;) {
        value.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
    return value;
}

std::filesystem::path module_file(
    const ClassicObject &object, std::string_view attribute_name,
    const std::filesystem::path &description_directory,
    std::string_view module_name, std::string_view patch_name,
    std::string_view extension) {
    const ClassicAttribute *attribute = find_classic_attribute(
        object.attributes, attribute_name);
    if (!attribute || attribute->value.empty()) {
        fail("missing " + std::string{attribute_name} + " for " +
             std::string{module_name});
    }
    std::string value = replace_all(
        attribute->value, "${FXMODULE}", module_name);
    constexpr std::string_view desc{"${DESC}"};
    std::filesystem::path path;
    if (value.starts_with(desc)) {
        value.erase(0u, desc.size());
        while (!value.empty() && (value.front() == '/' || value.front() == '\\')) {
            value.erase(value.begin());
        }
        path = description_directory / value;
    } else {
        path = value;
    }
    if (path.extension() != extension) {
        path /= std::string{patch_name} + std::string{extension};
    }
    if (!std::filesystem::is_regular_file(path)) {
        fail("module file does not exist: " + path.string());
    }
    return path;
}

std::uint32_t byte_channel(float value) {
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
        fail("PTEX clump ID channel is out of range");
    }
    return static_cast<std::uint32_t>(std::lround(value * 255.0f));
}

std::uint32_t decode_rgb_id(std::array<std::uint32_t, 3u> channels) noexcept {
    if (channels[0] == 0u && channels[1] == 0u && channels[2] == 0u) {
        return 0u;
    }
    std::uint32_t result = 0u;
    std::uint32_t channel = 2u;
    for (std::uint32_t bit = 0u; bit < 24u; ++bit) {
        result = result * 2u + ((~channels[channel]) & 1u);
        channels[channel] >>= 1u;
        channel = channel == 0u ? 2u : channel - 1u;
    }
    return result;
}

} // namespace

ClassicClumpRuntimeData build_xgen_classic_clump_runtime_data(
    const ClassicDescription &description,
    const ClassicAlembicAssetInput &surface,
    const std::filesystem::path &description_directory,
    const ClassicRootPlan &strand_roots,
    const ClassicFloatRuntimePlan &runtime_plan,
    std::size_t module_index,
    std::uint32_t cvs_per_guide) {
    if (module_index >= runtime_plan.clumps.size()) {
        fail("runtime module index is out of range");
    }
    const ClassicFloatClumpModule &module = runtime_plan.clumps[module_index];
    if (cvs_per_guide < 3u || description.patches.size() != 1u ||
        !surface.reference_surface) {
        fail("binding needs one subdivision patch and at least three CVs");
    }
    const ClassicPatch &patch = description.patches.front();
    const ClassicObject &object = find_module(description, module.name);
    const std::filesystem::path xpd_path = module_file(
        object, "pointDir", description_directory, module.name,
        patch.name, ".xuv");
    const std::filesystem::path ptex_path = module_file(
        object, "mapDir", description_directory, module.name,
        patch.name, ".ptx");
    const XpdDocument xpd = load_xpd_document(xpd_path);
    if (xpd.primitive_type != XpdPrimitiveType::Point ||
        xpd.coordinate_space != XpdCoordinateSpace::World ||
        xpd.cv_count != 1u) {
        fail("clump point file has an unsupported schema");
    }
    const auto block = std::find_if(
        xpd.blocks.begin(), xpd.blocks.end(), [](const XpdBlockInfo &entry) {
            return entry.name == "Location";
        });
    if (block == xpd.blocks.end() || block->floats_per_primitive != 6u) {
        fail("clump point file needs a six-float Location block");
    }
    const std::size_t block_index = static_cast<std::size_t>(
        block - xpd.blocks.begin());
    std::vector<ClassicExplicitRoot> valid_samples;
    std::vector<std::uint32_t> global_to_compact;
    std::array<float, 6u> record{};
    for (std::size_t face_index = 0u; face_index < xpd.faces.size();
         ++face_index) {
        const XpdFaceInfo &face = xpd.faces[face_index];
        if (face.face_id < 0) { continue; }
        for (std::uint32_t primitive = 0u;
             primitive < face.primitive_count; ++primitive) {
            copy_xpd_primitive(
                xpd, face_index, block_index, primitive, record);
            const float rounded = std::round(record[0]);
            if (!std::isfinite(record[0]) || record[0] < 0.0f ||
                std::abs(record[0] - rounded) > 1.0e-4f ||
                rounded > static_cast<float>(
                    std::numeric_limits<std::uint32_t>::max())) {
                fail("clump point primitive ID is invalid");
            }
            const ClassicReferenceSurfaceSample reference =
                surface.reference_surface->evaluate(
                    patch.name, static_cast<std::uint32_t>(face.face_id),
                    record[1], record[2]);
            bool associated = false;
            for (const GuideInput &guide : surface.asset.guides) {
                if (evaluate_xgen_classic_guide_weight(
                        guide, reference.position, reference.normal) > 1.0e-5f) {
                    associated = true;
                    break;
                }
            }
            if (!associated) {
                global_to_compact.push_back(kInvalidIndex);
                continue;
            }
            if (valid_samples.size() >=
                std::numeric_limits<std::uint32_t>::max()) {
                fail("valid clump guide count exceeds uint32");
            }
            global_to_compact.push_back(
                static_cast<std::uint32_t>(valid_samples.size()));
            valid_samples.push_back({
                static_cast<std::uint32_t>(face.face_id),
                {record[1], record[2]},
                {record[3], record[4], record[5]},
                static_cast<std::uint32_t>(rounded)});
        }
    }
    if (valid_samples.empty()) { fail("clump point file has no valid guides"); }
    const ClassicRootPlan guide_roots = build_xgen_classic_explicit_root_plan(
        description, surface, patch.name, valid_samples);
    PackedGeneratedCurves axes = generate_xgen_classic_base_curves_cpu(
        surface.asset, guide_roots, cvs_per_guide);
    ClassicFloatRuntimePlan prefix = runtime_plan;
    // Every clump guide is itself generated by the active primitive, so the
    // primitive length program applies even to the first ClumpingFX module.
    // Width/taper only affect renderer radii and must not enter the guide axis.
    // Keep width evaluation because XgSplinePrimitive uses the authored base
    // diameter as an early primitive-culling condition before applying FX;
    // the resulting radii are discarded when copying the guide axis below.
    prefix.taper.reset();
    prefix.taper_start.reset();
    prefix.width_ramp.reset();
    const auto current = std::find_if(
        prefix.effects.begin(), prefix.effects.end(),
        [module_index](const ClassicFloatEffect &effect) {
            return effect.type == ClassicFloatEffectType::Clump &&
                   effect.module_index == module_index;
        });
    if (current == prefix.effects.end()) {
        fail("runtime plan does not schedule the clump module");
    }
    prefix.effects.erase(current, prefix.effects.end());
    for (const ClassicFloatEffect effect : prefix.effects) {
        if (effect.type != ClassicFloatEffectType::Clump) {
            fail("non-clump effects before a clump guide are not yet supported");
        }
    }
    prefix.clumps.resize(module_index);
    std::vector<ClassicClumpRuntimeData> upstream;
    upstream.reserve(module_index);
    for (std::size_t previous = 0u; previous < module_index; ++previous) {
        upstream.push_back(build_xgen_classic_clump_runtime_data(
            description, surface, description_directory, guide_roots,
            runtime_plan, previous, cvs_per_guide));
    }
    const ClassicRuntimeInputData guide_inputs =
        build_xgen_classic_runtime_input_data(
            prefix, description_directory, patch.name, guide_roots);
    apply_xgen_classic_float_runtime_plan_cpu(
        axes, prefix, 1.0f, guide_roots.surface_tangents,
        guide_roots.random_prefixes, guide_roots.primitive_ids, upstream,
        guide_inputs.values, guide_roots.reference_positions);
    ClassicClumpRuntimeData result{};
    result.module_name = module.name;
    result.cvs_per_guide = cvs_per_guide;
    result.guide_axes.reserve(axes.points.size());
    for (const PackedCurvePoint &point : axes.points) {
        result.guide_axes.push_back({point.x, point.y, point.z});
    }
    result.guide_normals.reserve(axes.roots.size());
    result.guide_tangents.reserve(axes.roots.size());
    result.guide_reference_positions.reserve(axes.roots.size());
    result.guide_uvs.reserve(axes.roots.size());
    result.guide_face_ids.reserve(axes.roots.size());
    result.guide_random_prefixes.reserve(axes.roots.size());
    std::vector<std::uint32_t> valid_to_runtime(
        guide_roots.roots.size(), kInvalidIndex);
    std::size_t source_guide = 0u;
    const auto same_identity = [](const RootSample &a, const RootSample &b) {
        return a.surface_face_id == b.surface_face_id &&
               std::bit_cast<std::uint32_t>(a.uv.x) ==
                   std::bit_cast<std::uint32_t>(b.uv.x) &&
               std::bit_cast<std::uint32_t>(a.uv.y) ==
                   std::bit_cast<std::uint32_t>(b.uv.y);
    };
    for (std::size_t guide = 0u; guide < axes.roots.size(); ++guide) {
        while (source_guide < guide_roots.roots.size() &&
               !same_identity(axes.roots[guide],
                              guide_roots.roots[source_guide])) {
            ++source_guide;
        }
        if (source_guide == guide_roots.roots.size()) {
            fail("culled guide identity was not preserved");
        }
        valid_to_runtime[source_guide] = static_cast<std::uint32_t>(guide);
        result.guide_normals.push_back(
            guide_roots.roots[source_guide].normal);
        result.guide_tangents.push_back(
            guide_roots.surface_tangents[source_guide]);
        result.guide_reference_positions.push_back(
            guide_roots.reference_positions[source_guide]);
        result.guide_uvs.push_back(guide_roots.roots[source_guide].uv);
        result.guide_face_ids.push_back(
            guide_roots.roots[source_guide].surface_face_id);
        result.guide_random_prefixes.push_back(
            guide_roots.random_prefixes[source_guide]);
        ++source_guide;
    }
    for (std::uint32_t &guide : global_to_compact) {
        if (guide == kInvalidIndex) { continue; }
        if (guide >= valid_to_runtime.size()) {
            fail("clump guide remap is out of range");
        }
        guide = valid_to_runtime[guide];
    }

    const XgenPtexMap map{ptex_path};
    if (map.info().channel_count < 3u) {
        fail("clump ID map needs at least three channels");
    }
    XgenPtexSampleOptions options{};
    options.filter = XgenPtexFilter::Point;
    result.strand_guide_indices.reserve(strand_roots.roots.size());
    for (const RootSample &root : strand_roots.roots) {
        if (root.surface_face_id >= map.info().face_count) {
            fail("clump ID map has fewer faces than the patch");
        }
        std::array<std::uint32_t, 3u> channels{};
        for (std::uint32_t channel = 0u; channel < channels.size(); ++channel) {
            channels[channel] = byte_channel(map.sample(
                root.surface_face_id, root.uv.x, root.uv.y,
                channel, options));
        }
        const std::uint32_t encoded = decode_rgb_id(channels);
        if (encoded == 0u) {
            result.strand_guide_indices.push_back(kInvalidIndex);
            continue;
        }
        const std::uint32_t global = encoded - 1u;
        if (global >= global_to_compact.size()) {
            fail("clump ID map references a missing XPD guide");
        }
        result.strand_guide_indices.push_back(global_to_compact[global]);
    }
    return result;
}

} // namespace nanoxgen
