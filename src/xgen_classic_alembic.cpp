#include "nanoxgen/xgen_classic_alembic.h"

#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>
#include <opensubdiv/far/patchMap.h>
#include <opensubdiv/far/patchTableFactory.h>
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/ptexIndices.h>
#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nanoxgen {
namespace {

namespace Abc = Alembic::Abc;
namespace AbcGeom = Alembic::AbcGeom;
namespace AbcFactory = Alembic::AbcCoreFactory;
namespace AbcCore = Alembic::AbcCoreAbstract;
namespace Far = OpenSubdiv::Far;
namespace Sdc = OpenSubdiv::Sdc;

struct LoadedMesh {
    std::vector<Imath::V3f> positions;
    std::vector<Imath::V3f> reference_positions;
    std::vector<std::int32_t> face_counts;
    std::vector<std::int32_t> face_indices;
    Imath::M44d transform{};
};

struct ArchiveSampleSelection {
    bool first_sample{true};
    double time_seconds{};
    ClassicAlembicInterpolation interpolation{
        ClassicAlembicInterpolation::None};
};

struct SubdPosition {
    float x{};
    float y{};
    float z{};

    void Clear() noexcept { x = y = z = 0.0f; }
    void AddWithWeight(const SubdPosition &source, float weight) noexcept {
        x += source.x * weight;
        y += source.y * weight;
        z += source.z * weight;
    }
};

struct Double3 {
    double x{};
    double y{};
    double z{};
};

struct SubdSample {
    Vec3 position{};
    Vec3 du{};
    Vec3 dv{};
};

struct SurfaceFrame {
    Vec3 normal{};
    Vec3 tangent{};
    Vec3 binormal{};
};

SurfaceFrame xgen_surface_frame(Vec3 du, Vec3 dv) {
    // XGen's patch v increases in the opposite direction from OpenSubdiv's
    // Ptex v. XgPatch::evalFrame then rotates the two unit parameter tangents
    // away from one another by half of their deviation from 90 degrees. The
    // normalized sum/difference form performs the same symmetric
    // orthogonalization without trigonometry.
    using D3 = std::array<double, 3u>;
    const auto add = [](D3 a, D3 b) {
        return D3{a[0] + b[0], a[1] + b[1], a[2] + b[2]};
    };
    const auto subtract = [](D3 a, D3 b) {
        return D3{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    const auto scale = [](D3 value, double amount) {
        return D3{
            value[0] * amount, value[1] * amount, value[2] * amount};
    };
    const auto normalized = [&](D3 value) {
        const double length_squared =
            value[0] * value[0] + value[1] * value[1] +
            value[2] * value[2];
        if (!(length_squared > 1.0e-30)) {
            throw std::runtime_error(
                "Classic Alembic import: zero surface parameter tangent");
        }
        return scale(value, 1.0 / std::sqrt(length_squared));
    };
    const auto crossed = [](D3 a, D3 b) {
        return D3{
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
    };
    const D3 u_tangent = normalized({
        static_cast<double>(du.x), static_cast<double>(du.y),
        static_cast<double>(du.z)});
    const D3 v_tangent = normalized({
        -static_cast<double>(dv.x), -static_cast<double>(dv.y),
        -static_cast<double>(dv.z)});
    const D3 sum = add(u_tangent, v_tangent);
    const D3 difference = subtract(u_tangent, v_tangent);
    const double sum_length_squared =
        sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2];
    const double difference_length_squared =
        difference[0] * difference[0] +
        difference[1] * difference[1] +
        difference[2] * difference[2];
    if (!(sum_length_squared > 1.0e-30) ||
        !(difference_length_squared > 1.0e-30)) {
        throw std::runtime_error(
            "Classic Alembic import: surface parameter tangents cannot form an XGen frame");
    }
    constexpr double kInverseSqrtTwo = 0.707106781186547524400844362104849;
    const D3 tangent = scale(
        add(normalized(sum), normalized(difference)), kInverseSqrtTwo);
    const D3 binormal = scale(
        subtract(normalized(sum), normalized(difference)), kInverseSqrtTwo);
    const D3 normal = normalized(crossed(tangent, binormal));
    const auto to_float = [](D3 value) {
        return Vec3{
            static_cast<float>(value[0]), static_cast<float>(value[1]),
            static_cast<float>(value[2])};
    };
    return {to_float(normal), to_float(tangent), to_float(binormal)};
}

Double3 to_double(Vec3 value) noexcept {
    return {static_cast<double>(value.x), static_cast<double>(value.y),
            static_cast<double>(value.z)};
}

Double3 operator-(Double3 lhs, Double3 rhs) noexcept {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Double3 operator+(Double3 lhs, Double3 rhs) noexcept {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Double3 operator*(Double3 value, double scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}

Double3 cross(Double3 lhs, Double3 rhs) noexcept {
    return {lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x};
}

double length(Double3 value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y +
                     value.z * value.z);
}

double triangle_double_area(Double3 a, Double3 b, Double3 c) noexcept {
    return length(cross(b - a, c - a));
}

// SgSubdSurface::area evaluates the float control cage, not the OpenSubdiv
// limit surface. For a quad it averages the double-areas of all four corner
// triples. This is symmetric for non-planar cages and equals the usual area
// for a planar quad.
double xgen_quad_area(const Double3 (&p)[4]) noexcept {
    return 0.25 * (triangle_double_area(p[0], p[1], p[2]) +
                   triangle_double_area(p[0], p[1], p[3]) +
                   triangle_double_area(p[0], p[2], p[3]) +
                   triangle_double_area(p[1], p[2], p[3]));
}

// SgSubdSurface::lengthU/V use straight spans between opposite control-cage
// edges. XGen requests both at 0.5 for surface compensation.
double xgen_quad_length_u(const Double3 (&p)[4]) noexcept {
    return length((p[3] + p[2]) * 0.5 - (p[0] + p[1]) * 0.5);
}

double xgen_quad_length_v(const Double3 (&p)[4]) noexcept {
    return length((p[1] + p[2]) * 0.5 - (p[0] + p[3]) * 0.5);
}

struct MeshSearch {
    std::string target;
    std::size_t visited{};
    std::size_t max_objects{};
    ArchiveSampleSelection selection;
    std::vector<std::pair<Abc::IObject, Imath::M44d>> matches;
};

struct CurveSearch {
    std::size_t visited{};
    std::size_t max_objects{};
    ArchiveSampleSelection selection;
    std::vector<std::pair<Abc::IObject, Imath::M44d>> matches;
};

void find_curves(const Abc::IObject &object,
                 const Imath::M44d &parent_transform,
                 CurveSearch &search);

void inspect_static_curves(
    const Abc::IObject &object, bool animated_ancestor,
    std::size_t &visited, std::size_t max_objects,
    std::size_t &matches, bool &is_static);

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error("Classic Alembic import: " + message);
}

bool finite(const Imath::V3d &value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

struct SampleBlend {
    AbcCore::index_t lower{};
    AbcCore::index_t upper{};
    double alpha{};
};

template<typename Schema>
SampleBlend select_samples(
    const Schema &schema, const ArchiveSampleSelection &selection) {
    const std::size_t sample_count = schema.getNumSamples();
    if (sample_count == 0u) { fail("schema has no samples"); }
    if (selection.first_sample || sample_count == 1u) {
        return {};
    }
    const auto sampling = schema.getTimeSampling();
    if (!sampling) { fail("schema has no time sampling"); }
    const auto lower = sampling->getFloorIndex(
        selection.time_seconds,
        static_cast<AbcCore::index_t>(sample_count));
    if (selection.interpolation == ClassicAlembicInterpolation::None) {
        return {lower.first, lower.first, 0.0};
    }
    const auto upper = sampling->getCeilIndex(
        selection.time_seconds,
        static_cast<AbcCore::index_t>(sample_count));
    if (lower.first == upper.first || !(upper.second > lower.second)) {
        return {lower.first, lower.first, 0.0};
    }
    const double alpha = std::clamp(
        (selection.time_seconds - lower.second) /
            (upper.second - lower.second),
        0.0, 1.0);
    return {lower.first, upper.first, alpha};
}

Imath::M44d sample_transform(
    const AbcGeom::IXformSchema &schema,
    const ArchiveSampleSelection &selection) {
    const SampleBlend blend = select_samples(schema, selection);
    AbcGeom::XformSample lower;
    schema.get(lower, Abc::ISampleSelector{blend.lower});
    Imath::M44d result = lower.getMatrix();
    if (blend.lower == blend.upper) { return result; }
    AbcGeom::XformSample upper;
    schema.get(upper, Abc::ISampleSelector{blend.upper});
    if (!lower.isTopologyEqual(upper) ||
        lower.getInheritsXforms() != upper.getInheritsXforms()) {
        fail("transform topology changes between motion samples");
    }
    AbcGeom::XformSample interpolated;
    interpolated.setInheritsXforms(lower.getInheritsXforms());
    for (std::size_t op_index = 0u;
         op_index < lower.getNumOps(); ++op_index) {
        AbcGeom::XformOp op = lower.getOp(op_index);
        const AbcGeom::XformOp upper_op = upper.getOp(op_index);
        for (std::size_t channel = 0u;
             channel < op.getNumChannels(); ++channel) {
            const double value =
                op.getChannelValue(channel) * (1.0 - blend.alpha) +
                upper_op.getChannelValue(channel) * blend.alpha;
            if (!std::isfinite(value)) {
                fail("transform interpolation produced a non-finite value");
            }
            op.setChannelValue(channel, value);
        }
        interpolated.addOp(op);
    }
    return interpolated.getMatrix();
}

void find_meshes(const Abc::IObject &object, const Imath::M44d &parent_transform,
                 bool inside_target, MeshSearch &search) {
    if (++search.visited > search.max_objects) {
        fail("object limit exceeded");
    }
    Imath::M44d transform = parent_transform;
    if (AbcGeom::IXform::matches(object.getHeader())) {
        AbcGeom::IXform xform{object, Abc::kWrapExisting};
        if (xform.getSchema().getNumSamples() != 0u) {
            transform = sample_transform(
                xform.getSchema(), search.selection) * parent_transform;
        }
    }
    const bool selected = inside_target || object.getName() == search.target;
    if (selected && (AbcGeom::IPolyMesh::matches(object.getHeader()) ||
                     AbcGeom::ISubD::matches(object.getHeader()))) {
        search.matches.emplace_back(object, transform);
    }
    for (std::size_t index = 0u; index < object.getNumChildren(); ++index) {
        const Abc::ObjectHeader &header = object.getChildHeader(index);
        find_meshes(Abc::IObject{object, header.getName()}, transform,
                    selected, search);
    }
}

void inspect_static_deformation(
    const Abc::IObject &object, std::string_view target,
    bool inside_target, bool animated_ancestor,
    std::size_t &visited, std::size_t max_objects,
    std::size_t &matches, bool &is_static) {
    if (++visited > max_objects) { fail("object limit exceeded"); }
    bool animated = animated_ancestor;
    if (AbcGeom::IXform::matches(object.getHeader())) {
        AbcGeom::IXform xform{object, Abc::kWrapExisting};
        animated |= xform.getSchema().getNumSamples() > 1u;
    }
    const bool selected = inside_target || object.getName() == target;
    if (selected && (AbcGeom::IPolyMesh::matches(object.getHeader()) ||
                     AbcGeom::ISubD::matches(object.getHeader()))) {
        ++matches;
        std::size_t samples{};
        if (AbcGeom::IPolyMesh::matches(object.getHeader())) {
            AbcGeom::IPolyMesh mesh{object, Abc::kWrapExisting};
            samples = mesh.getSchema().getNumSamples();
        } else {
            AbcGeom::ISubD mesh{object, Abc::kWrapExisting};
            samples = mesh.getSchema().getNumSamples();
        }
        is_static &= !animated && samples <= 1u;
    }
    for (std::size_t index = 0u; index < object.getNumChildren(); ++index) {
        const Abc::ObjectHeader &header = object.getChildHeader(index);
        inspect_static_deformation(
            Abc::IObject{object, header.getName()}, target, selected,
            animated, visited, max_objects, matches, is_static);
    }
}

template<typename Sample>
LoadedMesh copy_mesh_sample(const Sample &sample, const Imath::M44d &transform,
                            const ClassicAlembicLimits &limits) {
    const auto positions = sample.getPositions();
    const auto counts = sample.getFaceCounts();
    const auto indices = sample.getFaceIndices();
    if (!positions || !counts || !indices) {
        fail("mesh sample is missing positions or topology");
    }
    if (positions->size() == 0u || positions->size() > limits.max_vertices) {
        fail("vertex count is empty or exceeds the limit");
    }
    if (counts->size() == 0u || counts->size() > limits.max_faces) {
        fail("face count is empty or exceeds the limit");
    }
    if (indices->size() > limits.max_face_vertices) {
        fail("face-vertex count exceeds the limit");
    }
    LoadedMesh result{};
    result.positions.assign(positions->get(), positions->get() + positions->size());
    result.reference_positions = result.positions;
    result.face_counts.assign(counts->get(), counts->get() + counts->size());
    result.face_indices.assign(indices->get(), indices->get() + indices->size());
    result.transform = transform;
    return result;
}

template<typename Schema, typename Sample>
LoadedMesh load_mesh_sample(
    const Schema &schema, const Imath::M44d &transform,
    const ClassicAlembicLimits &limits,
    const ArchiveSampleSelection &selection) {
    const SampleBlend blend = select_samples(schema, selection);
    Sample lower;
    schema.get(lower, Abc::ISampleSelector{blend.lower});
    LoadedMesh result = copy_mesh_sample(lower, transform, limits);
    if (blend.lower == blend.upper) { return result; }
    Sample upper;
    schema.get(upper, Abc::ISampleSelector{blend.upper});
    const auto upper_positions = upper.getPositions();
    const auto upper_counts = upper.getFaceCounts();
    const auto upper_indices = upper.getFaceIndices();
    if (!upper_positions || !upper_counts || !upper_indices ||
        upper_positions->size() != result.positions.size() ||
        upper_counts->size() != result.face_counts.size() ||
        upper_indices->size() != result.face_indices.size() ||
        !std::equal(
            upper_counts->get(),
            upper_counts->get() + upper_counts->size(),
            result.face_counts.begin()) ||
        !std::equal(
            upper_indices->get(),
            upper_indices->get() + upper_indices->size(),
            result.face_indices.begin())) {
        fail("mesh topology changes between motion samples");
    }
    const float alpha = static_cast<float>(blend.alpha);
    for (std::size_t index = 0u; index < result.positions.size(); ++index) {
        const Imath::V3f a = result.positions[index];
        const Imath::V3f b = (*upper_positions)[index];
        const Imath::V3f value = a * (1.0f - alpha) + b * alpha;
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z)) {
            fail("position interpolation produced a non-finite value");
        }
        result.positions[index] = value;
    }
    return result;
}

template<typename Schema>
void load_reference_positions(const Schema &schema, LoadedMesh &mesh) {
    const Abc::ICompoundProperty arbitrary = schema.getArbGeomParams();
    if (!arbitrary.valid() ||
        !arbitrary.getPropertyHeader("xgen_Pref")) {
        return;
    }
    const AbcGeom::IV3dGeomParam pref{arbitrary, "xgen_Pref"};
    const AbcGeom::IV3dGeomParam::Sample sample = pref.getExpandedValue(
        Abc::ISampleSelector{AbcCore::index_t{0}});
    const auto values = sample.getVals();
    if (!values || values->size() != mesh.positions.size()) {
        fail("xgen_Pref must contain one value per mesh vertex");
    }
    mesh.reference_positions.resize(values->size());
    for (std::size_t index = 0u; index < values->size(); ++index) {
        const Imath::V3d &value = (*values)[index];
        if (!finite(value)) { fail("xgen_Pref contains a non-finite value"); }
        const Imath::V3f converted{
            static_cast<float>(value.x), static_cast<float>(value.y),
            static_cast<float>(value.z)};
        if (!std::isfinite(converted.x) || !std::isfinite(converted.y) ||
            !std::isfinite(converted.z)) {
            fail("xgen_Pref value cannot be represented as float");
        }
        mesh.reference_positions[index] = converted;
    }
}

LoadedMesh load_patch_mesh(const Abc::IArchive &archive, std::string_view name,
                           const ClassicAlembicLimits &limits,
                           const ArchiveSampleSelection &selection) {
    MeshSearch search{
        std::string{name}, 0u, limits.max_objects, selection, {}};
    find_meshes(archive.getTop(), Imath::M44d{}, false, search);
    if (search.matches.empty()) {
        fail("patch object not found: " + std::string{name});
    }
    if (search.matches.size() != 1u) {
        fail("patch object resolves to multiple meshes: " + std::string{name});
    }
    const auto &[object, transform] = search.matches.front();
    if (AbcGeom::IPolyMesh::matches(object.getHeader())) {
        AbcGeom::IPolyMesh mesh{object, Abc::kWrapExisting};
        if (mesh.getSchema().getNumSamples() == 0u) {
            fail("polygon mesh has no samples: " + object.getFullName());
        }
        LoadedMesh result =
            load_mesh_sample<
                AbcGeom::IPolyMeshSchema,
                AbcGeom::IPolyMeshSchema::Sample>(
                mesh.getSchema(), transform, limits, selection);
        load_reference_positions(mesh.getSchema(), result);
        return result;
    }
    AbcGeom::ISubD mesh{object, Abc::kWrapExisting};
    if (mesh.getSchema().getNumSamples() == 0u) {
        fail("subdivision mesh has no samples: " + object.getFullName());
    }
    LoadedMesh result =
        load_mesh_sample<AbcGeom::ISubDSchema, AbcGeom::ISubDSchema::Sample>(
            mesh.getSchema(), transform, limits, selection);
    load_reference_positions(mesh.getSchema(), result);
    return result;
}

std::vector<std::vector<Vec3>> load_guide_cache_curves(
    const Abc::IArchive &archive,
    const ClassicAlembicLimits &limits,
    const ArchiveSampleSelection &selection,
    std::vector<std::string> &curve_names) {
    CurveSearch search{0u, limits.max_objects, selection, {}};
    find_curves(archive.getTop(), Imath::M44d{}, search);
    if (search.matches.empty()) {
        fail("guide cache contains no ICurves objects");
    }

    std::vector<std::vector<Vec3>> result;
    std::size_t total_points{};
    for (const auto &[object, transform] : search.matches) {
        AbcGeom::ICurves curves{object, Abc::kWrapExisting};
        const auto &schema = curves.getSchema();
        if (schema.getNumSamples() == 0u) {
            fail("guide-cache curves have no samples: " +
                 object.getFullName());
        }
        const SampleBlend blend = select_samples(schema, selection);
        AbcGeom::ICurvesSchema::Sample lower;
        schema.get(lower, Abc::ISampleSelector{blend.lower});
        const auto lower_positions = lower.getPositions();
        const auto lower_counts = lower.getCurvesNumVertices();
        if (!lower_positions || !lower_counts) {
            fail("guide-cache curves are missing positions or counts: " +
                 object.getFullName());
        }

        Abc::P3fArraySamplePtr upper_positions;
        if (blend.lower != blend.upper) {
            AbcGeom::ICurvesSchema::Sample upper;
            schema.get(upper, Abc::ISampleSelector{blend.upper});
            upper_positions = upper.getPositions();
            const auto upper_counts = upper.getCurvesNumVertices();
            if (!upper_positions || !upper_counts ||
                upper_positions->size() != lower_positions->size() ||
                upper_counts->size() != lower_counts->size() ||
                !std::equal(
                    upper_counts->get(),
                    upper_counts->get() + upper_counts->size(),
                    lower_counts->get())) {
                fail("guide-cache curve topology changes between motion samples");
            }
        }

        std::size_t point_offset{};
        for (std::size_t curve = 0u; curve < lower_counts->size(); ++curve) {
            const std::int32_t raw_count = (*lower_counts)[curve];
            if (raw_count < 2) {
                fail("guide-cache curve has fewer than two CVs");
            }
            const std::size_t count = static_cast<std::size_t>(raw_count);
            if (point_offset > lower_positions->size() ||
                count > lower_positions->size() - point_offset) {
                fail("guide-cache curve counts exceed the position array");
            }
            if (total_points > limits.max_vertices ||
                count > limits.max_vertices - total_points) {
                fail("guide-cache point limit exceeded");
            }
            if (result.size() >= limits.max_faces) {
                fail("guide-cache curve limit exceeded");
            }
            std::vector<Vec3> output;
            output.reserve(count);
            for (std::size_t cv = 0u; cv < count; ++cv) {
                Imath::V3f value = (*lower_positions)[point_offset + cv];
                if (upper_positions) {
                    const float alpha = static_cast<float>(blend.alpha);
                    value = value * (1.0f - alpha) +
                        (*upper_positions)[point_offset + cv] * alpha;
                }
                Imath::V3d transformed;
                transform.multVecMatrix(
                    Imath::V3d{value.x, value.y, value.z}, transformed);
                if (!finite(transformed)) {
                    fail("guide-cache curve contains a non-finite position");
                }
                const Vec3 converted{
                    static_cast<float>(transformed.x),
                    static_cast<float>(transformed.y),
                    static_cast<float>(transformed.z)};
                if (!std::isfinite(converted.x) ||
                    !std::isfinite(converted.y) ||
                    !std::isfinite(converted.z)) {
                    fail("guide-cache position cannot be represented as float");
                }
                output.push_back(converted);
            }
            result.emplace_back(std::move(output));
            curve_names.push_back(object.getName());
            total_points += count;
            point_offset += count;
        }
        if (point_offset != lower_positions->size()) {
            fail("guide-cache curve counts do not consume the position array");
        }
    }
    return result;
}

Vec3 transformed_position(std::span<const Imath::V3f> positions,
                          const Imath::M44d &transform,
                          std::uint32_t index) {
    if (index >= positions.size()) { fail("face vertex index is out of range"); }
    const Imath::V3f &source = positions[index];
    Imath::V3d transformed;
    transform.multVecMatrix(
        Imath::V3d{source.x, source.y, source.z}, transformed);
    if (!finite(transformed)) { fail("mesh contains a non-finite position"); }
    const Vec3 result{static_cast<float>(transformed.x),
                      static_cast<float>(transformed.y),
                      static_cast<float>(transformed.z)};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z)) {
        fail("mesh position cannot be represented as float");
    }
    return result;
}

Vec3 position(const LoadedMesh &mesh, std::uint32_t index) {
    return transformed_position(mesh.positions, mesh.transform, index);
}

Vec3 reference_position(const LoadedMesh &mesh, std::uint32_t index) {
    return transformed_position(
        mesh.reference_positions, mesh.transform, index);
}

struct PtexFaceLocation {
    std::uint32_t coarse_face{};
    std::uint32_t quadrant{};
    std::uint32_t quadrant_count{};
};

std::vector<PtexFaceLocation> build_ptex_face_locations(
    const LoadedMesh &mesh) {
    if (mesh.face_counts.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        fail("subdivision face count exceeds uint32");
    }
    std::vector<PtexFaceLocation> result;
    for (std::uint32_t face = 0u; face < mesh.face_counts.size(); ++face) {
        const std::int32_t raw_count = mesh.face_counts[face];
        if (raw_count < 3) {
            fail("mesh contains a face with fewer than three vertices");
        }
        const std::uint32_t count = static_cast<std::uint32_t>(raw_count);
        const std::uint32_t ptex_count = count == 4u ? 1u : count;
        if (ptex_count >
            std::numeric_limits<std::uint32_t>::max() - result.size()) {
            fail("subdivision PTEX face count exceeds uint32");
        }
        for (std::uint32_t quadrant = 0u;
             quadrant < ptex_count; ++quadrant) {
            result.push_back({face, quadrant, ptex_count});
        }
    }
    return result;
}

class SubdEvaluator {
public:
    SubdEvaluator(const LoadedMesh &mesh,
                  std::span<const std::uint32_t> selected_faces) {
        if (mesh.positions.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()) ||
            mesh.face_counts.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()) ||
            mesh.face_indices.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()) ||
            selected_faces.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max())) {
            fail("subdivision topology exceeds int indexing");
        }
        std::vector<int> counts(mesh.face_counts.begin(), mesh.face_counts.end());
        std::vector<int> indices;
        indices.reserve(mesh.face_indices.size());
        std::size_t face_offset{};
        for (const std::int32_t raw_count : mesh.face_counts) {
            if (raw_count < 3) {
                fail("mesh contains a face with fewer than three vertices");
            }
            const std::size_t count = static_cast<std::size_t>(raw_count);
            if (face_offset > mesh.face_indices.size() ||
                count > mesh.face_indices.size() - face_offset) {
                fail("face counts exceed the face-index array");
            }
            for (std::size_t corner = 0u; corner < count; ++corner) {
                indices.push_back(mesh.face_indices[
                    face_offset + count - 1u - corner]);
            }
            face_offset += count;
        }
        if (face_offset != mesh.face_indices.size()) {
            fail("face counts do not consume the face-index array");
        }
        Far::TopologyDescriptor descriptor{};
        descriptor.numVertices = static_cast<int>(mesh.positions.size());
        descriptor.numFaces = static_cast<int>(mesh.face_counts.size());
        descriptor.numVertsPerFace = counts.data();
        descriptor.vertIndicesPerFace = indices.data();
        Sdc::Options scheme_options;
        scheme_options.SetVtxBoundaryInterpolation(
            Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
        typename Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options
            refiner_options{Sdc::SCHEME_CATMARK, scheme_options};
        refiner_options.validateFullTopology = true;
        _base_refiner.reset(
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
                descriptor, refiner_options));
        if (!_base_refiner) { fail("OpenSubdiv rejected the patch topology"); }
        _ptex_faces = build_ptex_face_locations(mesh);

        _base_positions.resize(mesh.positions.size());
        _reference_base_positions.resize(mesh.positions.size());
        for (std::uint32_t index = 0u; index < mesh.positions.size(); ++index) {
            const Vec3 value = position(mesh, index);
            _base_positions[index] = {value.x, value.y, value.z};
            const Vec3 reference = reference_position(mesh, index);
            _reference_base_positions[index] = {
                reference.x, reference.y, reference.z};
        }
        std::vector<Far::Index> faces;
        faces.reserve(selected_faces.size());
        for (const std::uint32_t ptex_face : selected_faces) {
            if (ptex_face >= _ptex_faces.size()) {
                fail("selected subdivision PTEX face is out of range");
            }
            faces.push_back(static_cast<Far::Index>(
                _ptex_faces[ptex_face].coarse_face));
        }
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
        Far::ConstIndexArray group_faces{
            faces.data(), static_cast<int>(faces.size())};
        std::unique_ptr<Far::TopologyRefiner> local_refiner{
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
                *_base_refiner)};
        if (!local_refiner) { fail("cannot clone OpenSubdiv topology"); }
        Far::PatchTableFactory::Options patch_options{8u};
        patch_options.SetEndCapType(
            Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
        local_refiner->RefineAdaptive(
            patch_options.GetRefineAdaptiveOptions(), group_faces);
        _patch_table.reset(Far::PatchTableFactory::Create(
            *local_refiner, patch_options, group_faces));
        if (!_patch_table) { fail("cannot create OpenSubdiv patch table"); }
        _patch_map = std::make_unique<Far::PatchMap>(*_patch_table);

        const int base_vertices =
            local_refiner->GetLevel(0).GetNumVertices();
        const int refined_vertices =
            local_refiner->GetNumVerticesTotal() - base_vertices;
        const int local_points = _patch_table->GetNumLocalPoints();
        if (refined_vertices < 0 || local_points < 0) {
            fail("OpenSubdiv returned invalid local point counts");
        }
        _local_positions.resize(
            static_cast<std::size_t>(refined_vertices + local_points));
        _reference_local_positions.resize(
            static_cast<std::size_t>(refined_vertices + local_points));
        const auto refine = [&](const auto &base, auto &local) {
            if (refined_vertices != 0) {
                Far::PrimvarRefiner primvar_refiner{*local_refiner};
                const auto *source = base.data();
                auto *destination = local.data();
                for (int level = 1; level < local_refiner->GetNumLevels(); ++level) {
                    primvar_refiner.Interpolate(level, source, destination);
                    source = destination;
                    destination +=
                        local_refiner->GetLevel(level).GetNumVertices();
                }
            }
            if (local_points != 0) {
                const Far::StencilTable *stencils =
                    _patch_table->GetLocalPointStencilTable();
                if (!stencils) {
                    fail("OpenSubdiv local point stencils are missing");
                }
                stencils->UpdateValues(
                    base.data(), base_vertices, local.data(),
                    local.data() + refined_vertices);
            }
        };
        refine(_base_positions, _local_positions);
        refine(_reference_base_positions, _reference_local_positions);
    }

    [[nodiscard]] SubdSample evaluate(std::uint32_t face, float u, float v) const {
        return evaluate_positions(
            _base_positions, _local_positions, face, u, v);
    }

    [[nodiscard]] SubdSample evaluate_reference(
        std::uint32_t face, float u, float v) const {
        return evaluate_positions(
            _reference_base_positions, _reference_local_positions,
            face, u, v);
    }

private:
    [[nodiscard]] SubdSample evaluate_positions(
        const std::vector<SubdPosition> &base_positions,
        const std::vector<SubdPosition> &local_positions,
        std::uint32_t face, float u, float v) const {
        if (face >= _ptex_faces.size()) {
            fail("subdivision PTEX face is out of range");
        }
        // XGen logical subdivision face ids are OpenSubdiv Ptex face ids.
        // XGen's surface convention has V reversed relative to PatchMap.
        const float evaluation_u = u;
        const float evaluation_v = 1.0f - v;
        const Far::PatchTable::PatchHandle *handle =
            _patch_map->FindPatch(
                static_cast<int>(face),
                evaluation_u, evaluation_v);
        if (!handle) { fail("cannot locate OpenSubdiv patch for guide coordinate"); }
        float weights[20u]{};
        float du_weights[20u]{};
        float dv_weights[20u]{};
        _patch_table->EvaluateBasis(
            *handle, evaluation_u, evaluation_v,
            weights, du_weights, dv_weights);
        const Far::ConstIndexArray vertices =
            _patch_table->GetPatchVertices(*handle);
        if (vertices.size() <= 0 || vertices.size() > 20) {
            fail("unsupported OpenSubdiv patch control point count");
        }
        SubdPosition p{};
        SubdPosition du{};
        SubdPosition dv{};
        for (int index = 0; index < vertices.size(); ++index) {
            const Far::Index vertex = vertices[index];
            const SubdPosition *source = nullptr;
            if (vertex < static_cast<Far::Index>(base_positions.size())) {
                source = &base_positions[vertex];
            } else {
                const std::size_t local = static_cast<std::size_t>(vertex) -
                                          base_positions.size();
                if (local >= local_positions.size()) {
                    fail("OpenSubdiv patch control point is out of range");
                }
                source = &local_positions[local];
            }
            p.AddWithWeight(*source, weights[index]);
            du.AddWithWeight(*source, du_weights[index]);
            dv.AddWithWeight(*source, dv_weights[index]);
        }
        const SubdSample result{
            {p.x, p.y, p.z},
            {du.x, du.y, du.z},
            {-dv.x, -dv.y, -dv.z}};
        if (!std::isfinite(result.position.x) ||
            !std::isfinite(result.position.y) ||
            !std::isfinite(result.position.z) ||
            !std::isfinite(result.du.x) || !std::isfinite(result.du.y) ||
            !std::isfinite(result.du.z) || !std::isfinite(result.dv.x) ||
            !std::isfinite(result.dv.y) || !std::isfinite(result.dv.z)) {
            fail("OpenSubdiv evaluation produced a non-finite value");
        }
        return result;
    }

    std::unique_ptr<Far::TopologyRefiner> _base_refiner;
    std::unique_ptr<Far::PatchTable> _patch_table;
    std::unique_ptr<Far::PatchMap> _patch_map;
    std::vector<PtexFaceLocation> _ptex_faces;
    std::vector<SubdPosition> _base_positions;
    std::vector<SubdPosition> _local_positions;
    std::vector<SubdPosition> _reference_base_positions;
    std::vector<SubdPosition> _reference_local_positions;
};

SurfaceFrame subd_surface_frame(
    const SubdEvaluator &evaluator, std::uint32_t face, float u, float v,
    bool reference, const SubdSample &sample) {
    try {
        return xgen_surface_frame(sample.du, sample.dv);
    } catch (const std::runtime_error &) {
        // At an extraordinary corner the limit-surface parameterization can
        // have coincident analytic derivatives. XGen's level-one logical-face
        // evaluator still returns a frame there. Recover the same geometric
        // directions with independent one-sided U/V secants; a diagonal inset
        // keeps both derivatives on the singular parameter line.
        for (const float step : {1.0e-3f, 1.0e-2f}) {
            const float other_u = u <= 0.5f
                ? std::min(1.0f, u + step)
                : std::max(0.0f, u - step);
            const float other_v = v <= 0.5f
                ? std::min(1.0f, v + step)
                : std::max(0.0f, v - step);
            if (other_u == u || other_v == v) { continue; }
            const SubdSample u_sample = reference
                ? evaluator.evaluate_reference(face, other_u, v)
                : evaluator.evaluate(face, other_u, v);
            const SubdSample v_sample = reference
                ? evaluator.evaluate_reference(face, u, other_v)
                : evaluator.evaluate(face, u, other_v);
            const float inverse_u = 1.0f / (other_u - u);
            const float inverse_v = 1.0f / (other_v - v);
            const Vec3 finite_du{
                (u_sample.position.x - sample.position.x) * inverse_u,
                (u_sample.position.y - sample.position.y) * inverse_u,
                (u_sample.position.z - sample.position.z) * inverse_u};
            const Vec3 finite_dv{
                (v_sample.position.x - sample.position.x) * inverse_v,
                (v_sample.position.y - sample.position.y) * inverse_v,
                (v_sample.position.z - sample.position.z) * inverse_v};
            try {
                return xgen_surface_frame(finite_du, finite_dv);
            } catch (const std::runtime_error &) {
            }
        }
        fail("cannot construct frame for PTEX face " +
             std::to_string(face) + " at (" + std::to_string(u) +
             ", " + std::to_string(v) + ")");
    }
}

void find_curves(const Abc::IObject &object,
                 const Imath::M44d &parent_transform,
                 CurveSearch &search) {
    if (++search.visited > search.max_objects) {
        fail("guide-cache object limit exceeded");
    }
    Imath::M44d transform = parent_transform;
    if (AbcGeom::IXform::matches(object.getHeader())) {
        AbcGeom::IXform xform{object, Abc::kWrapExisting};
        if (xform.getSchema().getNumSamples() != 0u) {
            transform = sample_transform(
                xform.getSchema(), search.selection) * parent_transform;
        }
    }
    if (AbcGeom::ICurves::matches(object.getHeader())) {
        search.matches.emplace_back(object, transform);
    }
    for (std::size_t index = 0u; index < object.getNumChildren(); ++index) {
        const Abc::ObjectHeader &header = object.getChildHeader(index);
        find_curves(
            Abc::IObject{object, header.getName()}, transform, search);
    }
}

void inspect_static_curves(
    const Abc::IObject &object, bool animated_ancestor,
    std::size_t &visited, std::size_t max_objects,
    std::size_t &matches, bool &is_static) {
    if (++visited > max_objects) {
        fail("guide-cache object limit exceeded");
    }
    bool animated = animated_ancestor;
    if (AbcGeom::IXform::matches(object.getHeader())) {
        AbcGeom::IXform xform{object, Abc::kWrapExisting};
        animated |= xform.getSchema().getNumSamples() > 1u;
    }
    if (AbcGeom::ICurves::matches(object.getHeader())) {
        AbcGeom::ICurves curves{object, Abc::kWrapExisting};
        ++matches;
        is_static &= !animated && curves.getSchema().getNumSamples() <= 1u;
    }
    for (std::size_t index = 0u; index < object.getNumChildren(); ++index) {
        const Abc::ObjectHeader &header = object.getChildHeader(index);
        inspect_static_curves(
            Abc::IObject{object, header.getName()}, animated,
            visited, max_objects, matches, is_static);
    }
}

class ImportedReferenceSurface final : public ClassicReferenceSurfaceEvaluator {
public:
    void add(std::string patch_name,
             std::unique_ptr<SubdEvaluator> evaluator) {
        if (!evaluator ||
            !_evaluators.emplace(std::move(patch_name), std::move(evaluator)).second) {
            fail("duplicate reference-surface evaluator");
        }
    }

    [[nodiscard]] ClassicReferenceSurfaceSample evaluate_current(
        std::string_view patch_name, std::uint32_t face_id,
        float u, float v) const override {
        return evaluate_impl(patch_name, face_id, u, v, false);
    }

    [[nodiscard]] ClassicReferenceSurfaceSample evaluate(
        std::string_view patch_name, std::uint32_t face_id,
        float u, float v) const override {
        return evaluate_impl(patch_name, face_id, u, v, true);
    }

private:
    [[nodiscard]] ClassicReferenceSurfaceSample evaluate_impl(
        std::string_view patch_name, std::uint32_t face_id,
        float u, float v, bool reference) const {
        const auto found = _evaluators.find(std::string{patch_name});
        if (found == _evaluators.end()) {
            fail("reference-surface patch was not imported: " +
                 std::string{patch_name});
        }
        if (!std::isfinite(u) || !std::isfinite(v) ||
            u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
            fail("reference-surface coordinate is outside [0,1]");
        }
        const SubdSample sample = reference
            ? found->second->evaluate_reference(face_id, u, 1.0f - v)
            : found->second->evaluate(face_id, u, 1.0f - v);
        const SurfaceFrame frame = subd_surface_frame(
            *found->second, face_id, u, 1.0f - v, reference, sample);
        return {sample.position, frame.normal, frame.tangent};
    }
    std::unordered_map<std::string, std::unique_ptr<SubdEvaluator>> _evaluators;
};

struct FaceRange {
    std::size_t offset{};
    std::size_t count{};
    std::uint32_t first_triangle{};
    std::uint32_t first_vertex{};
};

std::vector<std::size_t> face_offsets(const LoadedMesh &mesh) {
    std::vector<std::size_t> result(mesh.face_counts.size() + 1u);
    for (std::size_t face = 0u; face < mesh.face_counts.size(); ++face) {
        const std::int32_t count = mesh.face_counts[face];
        if (count < 3) { fail("mesh contains a face with fewer than three vertices"); }
        const std::size_t next = result[face] + static_cast<std::size_t>(count);
        if (next > mesh.face_indices.size()) {
            fail("face counts exceed the face-index array");
        }
        result[face + 1u] = next;
    }
    if (result.back() != mesh.face_indices.size()) {
        fail("face counts do not consume the face-index array");
    }
    return result;
}

struct LimitVertexApproximation {
    std::vector<Vec3> positions;
    std::vector<bool> supported_neighborhood;
};

LimitVertexApproximation xgen_limit_vertices(const LoadedMesh &mesh) {
    const std::vector<std::size_t> offsets = face_offsets(mesh);
    struct Edge {
        std::uint32_t first{};
        std::uint32_t second{};
        std::uint32_t face_count{};
        Vec3 face_point_sum{};
        std::array<Vec3, 2u> adjacent_face_points{};
        std::uint32_t face_points_added{};
    };
    std::vector<Vec3> neighbor_sums(mesh.positions.size());
    std::vector<Vec3> boundary_neighbor_sums(mesh.positions.size());
    std::vector<Vec3> face_point_sums(mesh.positions.size());
    std::vector<std::uint32_t> incident_faces(mesh.positions.size());
    std::vector<std::uint32_t> boundary_edges(mesh.positions.size());
    std::vector<Vec3> face_points(mesh.face_counts.size());
    std::vector<Edge> edges;
    edges.reserve(mesh.face_indices.size() / 2u);
    std::unordered_map<std::uint64_t, std::size_t> edge_indices;
    edge_indices.reserve(mesh.face_indices.size());
    for (std::size_t face = 0u; face < mesh.face_counts.size(); ++face) {
        const std::size_t count = static_cast<std::size_t>(mesh.face_counts[face]);
        const std::size_t offset = offsets[face];
        Vec3 face_point{};
        for (std::size_t corner = 0u; corner < count; ++corner) {
            // Autodesk's Alembic bridge reverses each face's index run before
            // constructing SESubd. The winding does not change the limit
            // stencil mathematically, but it does change float accumulation
            // order in computeFacePoints and is observable in RandomGenerator
            // surface-compensation UVs.
            const std::size_t reversed = count - 1u - corner;
            const std::int32_t raw = mesh.face_indices[offset + reversed];
            const std::int32_t raw_next =
                mesh.face_indices[offset + (reversed + count - 1u) % count];
            if (raw < 0 || raw_next < 0) {
                fail("mesh contains a negative vertex index");
            }
            const std::uint32_t vertex = static_cast<std::uint32_t>(raw);
            const std::uint32_t next = static_cast<std::uint32_t>(raw_next);
            if (vertex >= mesh.positions.size() ||
                next >= mesh.positions.size()) {
                fail("mesh contains an out-of-range vertex index");
            }
            face_point = face_point + reference_position(mesh, vertex);
            const std::uint32_t first = std::min(vertex, next);
            const std::uint32_t second = std::max(vertex, next);
            const std::uint64_t key =
                (static_cast<std::uint64_t>(first) << 32u) | second;
            const auto [found, inserted] =
                edge_indices.try_emplace(key, edges.size());
            if (inserted) {
                edges.push_back({first, second, 1u});
            } else {
                ++edges[found->second].face_count;
            }
        }
        face_point = face_point / static_cast<float>(count);
        face_points[face] = face_point;
        for (std::size_t corner = 0u; corner < count; ++corner) {
            const std::size_t reversed = count - 1u - corner;
            const std::uint32_t vertex = static_cast<std::uint32_t>(
                mesh.face_indices[offset + reversed]);
            face_point_sums[vertex] = face_point_sums[vertex] + face_point;
            ++incident_faces[vertex];
            const std::uint32_t next = static_cast<std::uint32_t>(
                mesh.face_indices[offset + (reversed + count - 1u) % count]);
            const std::uint32_t first = std::min(vertex, next);
            const std::uint32_t second = std::max(vertex, next);
            const std::uint64_t key =
                (static_cast<std::uint64_t>(first) << 32u) | second;
            const auto found = edge_indices.find(key);
            if (found == edge_indices.end()) {
                fail("subdivision edge lookup is inconsistent");
            }
            Edge &edge = edges[found->second];
            if (edge.face_points_added < edge.adjacent_face_points.size()) {
                edge.adjacent_face_points[edge.face_points_added] = face_point;
            }
            ++edge.face_points_added;
            edge.face_point_sum = edge.face_point_sum + face_point;
        }
    }
    std::vector<std::uint32_t> valence(mesh.positions.size());
    for (const Edge &edge : edges) {
        if (edge.face_count != 2u) {
            boundary_neighbor_sums[edge.first] =
                boundary_neighbor_sums[edge.first] +
                reference_position(mesh, edge.second);
            boundary_neighbor_sums[edge.second] =
                boundary_neighbor_sums[edge.second] +
                reference_position(mesh, edge.first);
            ++boundary_edges[edge.first];
            ++boundary_edges[edge.second];
        }
        neighbor_sums[edge.first] = neighbor_sums[edge.first] +
            reference_position(mesh, edge.second);
        neighbor_sums[edge.second] = neighbor_sums[edge.second] +
            reference_position(mesh, edge.first);
        ++valence[edge.first];
        ++valence[edge.second];
    }
    // SESubd first updates the authored cage vertices once with the
    // Catmull-Clark vertex rule, then evaluates its limit stencil on that
    // smoothed cage. Applying the algebraically collapsed limit rule directly
    // to the authored cage changes float rounding and therefore XGen's
    // RandomGenerator surface-compensation UVs.
    std::vector<Vec3> smoothed_positions(mesh.positions.size());
    for (std::uint32_t vertex = 0u; vertex < mesh.positions.size(); ++vertex) {
        const Vec3 own = reference_position(mesh, vertex);
        const std::uint32_t n = valence[vertex];
        if (boundary_edges[vertex] == 2u) {
            smoothed_positions[vertex] =
                own * 0.75f + boundary_neighbor_sums[vertex] * 0.125f;
            continue;
        }
        const bool regular = boundary_edges[vertex] == 0u && n >= 3u &&
            incident_faces[vertex] == n;
        if (!regular) {
            smoothed_positions[vertex] = own;
            continue;
        }
        const float own_weight = static_cast<float>(
            (static_cast<double>(n) - 2.0) / static_cast<double>(n));
        const float sum_weight = static_cast<float>(
            1.0 / (static_cast<double>(n) * static_cast<double>(n)));
        smoothed_positions[vertex] =
            own * own_weight +
            (neighbor_sums[vertex] + face_point_sums[vertex]) * sum_weight;
    }

    std::vector<Vec3> edge_points(edges.size());
    for (std::size_t edge_index = 0u; edge_index < edges.size(); ++edge_index) {
        const Edge &edge = edges[edge_index];
        const Vec3 endpoints = reference_position(mesh, edge.first) +
            reference_position(mesh, edge.second);
        edge_points[edge_index] = edge.face_count == 2u
            ? (endpoints + edge.adjacent_face_points[0u] +
               edge.adjacent_face_points[1u]) * 0.25f
            : endpoints * 0.5f;
    }

    std::vector<Vec3> smoothed_neighbor_sums(mesh.positions.size());
    std::vector<Vec3> smoothed_boundary_neighbor_sums(mesh.positions.size());
    std::vector<Vec3> smoothed_face_point_sums(mesh.positions.size());
    std::vector<std::vector<std::size_t>> ordered_incident_edges(
        mesh.positions.size());
    for (std::size_t face = 0u; face < mesh.face_counts.size(); ++face) {
        const std::size_t count = static_cast<std::size_t>(mesh.face_counts[face]);
        const std::size_t offset = offsets[face];
        for (std::size_t corner = 0u; corner < count; ++corner) {
            const std::size_t reversed = count - 1u - corner;
            const std::uint32_t vertex = static_cast<std::uint32_t>(
                mesh.face_indices[offset + reversed]);
            const std::uint32_t previous = static_cast<std::uint32_t>(
                mesh.face_indices[offset + (reversed + 1u) % count]);
            const std::uint32_t next = static_cast<std::uint32_t>(
                mesh.face_indices[offset + (reversed + count - 1u) % count]);
            auto &incident = ordered_incident_edges[vertex];
            for (const std::uint32_t other : {next, previous}) {
                const std::uint32_t first = std::min(vertex, other);
                const std::uint32_t second = std::max(vertex, other);
                const std::uint64_t key =
                    (static_cast<std::uint64_t>(first) << 32u) | second;
                const auto found = edge_indices.find(key);
                if (found == edge_indices.end()) {
                    fail("subdivision edge lookup is inconsistent");
                }
                if (std::find(incident.begin(), incident.end(), found->second) ==
                    incident.end()) {
                    incident.push_back(found->second);
                }
            }
        }
    }
    for (std::uint32_t vertex = 0u; vertex < mesh.positions.size(); ++vertex) {
        for (const std::size_t edge_index : ordered_incident_edges[vertex]) {
            const Edge &edge = edges[edge_index];
            const Vec3 edge_point = edge_points[edge_index];
            smoothed_neighbor_sums[vertex] =
                smoothed_neighbor_sums[vertex] + edge_point;
            if (edge.face_count != 2u) {
                smoothed_boundary_neighbor_sums[vertex] =
                    smoothed_boundary_neighbor_sums[vertex] + edge_point;
            }
        }
    }
    for (std::size_t face = 0u; face < mesh.face_counts.size(); ++face) {
        const std::size_t count = static_cast<std::size_t>(mesh.face_counts[face]);
        const std::size_t offset = offsets[face];
        for (std::size_t corner = 0u; corner < count; ++corner) {
            const std::size_t reversed = count - 1u - corner;
            const std::uint32_t vertex = static_cast<std::uint32_t>(
                mesh.face_indices[offset + reversed]);
            const std::uint32_t previous = static_cast<std::uint32_t>(
                mesh.face_indices[offset + (reversed + 1u) % count]);
            const std::uint32_t next = static_cast<std::uint32_t>(
                mesh.face_indices[offset + (reversed + count - 1u) % count]);
            const auto find_edge_point = [&](std::uint32_t other) {
                const std::uint32_t first = std::min(vertex, other);
                const std::uint32_t second = std::max(vertex, other);
                const std::uint64_t key =
                    (static_cast<std::uint64_t>(first) << 32u) | second;
                const auto found = edge_indices.find(key);
                if (found == edge_indices.end()) {
                    fail("subdivision edge lookup is inconsistent");
                }
                return edge_points[found->second];
            };
            const Vec3 previous_edge_point = find_edge_point(previous);
            const Vec3 next_edge_point = find_edge_point(next);
            const Vec3 child_face_point =
                (smoothed_positions[vertex] + next_edge_point +
                 face_points[face] + previous_edge_point) * 0.25f;
            smoothed_face_point_sums[vertex] =
                smoothed_face_point_sums[vertex] + child_face_point;
        }
    }

    LimitVertexApproximation result{};
    result.positions.resize(mesh.positions.size());
    result.supported_neighborhood.resize(mesh.positions.size());
    for (std::uint32_t vertex = 0u; vertex < mesh.positions.size(); ++vertex) {
        const std::uint32_t n = valence[vertex];
        const Vec3 own = smoothed_positions[vertex];
        const bool boundary_corner =
            boundary_edges[vertex] == 2u && n == 2u;
        if (boundary_corner) {
            result.positions[vertex] = own;
            result.supported_neighborhood[vertex] = true;
            continue;
        }
        if (boundary_edges[vertex] == 2u) {
            // SESubd's smooth-boundary limit rule. It retains only the two
            // boundary neighbours in the vertex edge accumulator and applies
            // (4*P + E0 + E1) / 6 using float arithmetic.
            result.positions[vertex] =
                (own * 4.0f + smoothed_boundary_neighbor_sums[vertex]) *
                (1.0f / 6.0f);
            result.supported_neighborhood[vertex] = true;
            continue;
        }
        const bool regular = boundary_edges[vertex] == 0u && n >= 3u &&
            incident_faces[vertex] == n;
        if (!regular) {
            // SESubd marks non-smooth boundary junctions as corners and their
            // limit value is the authored cage vertex.
            result.positions[vertex] = own;
            result.supported_neighborhood[vertex] = true;
            continue;
        }
        result.supported_neighborhood[vertex] = true;
        const double dn = static_cast<double>(n);
        const float vertex_weight = static_cast<float>((dn - 1.0) / (dn + 5.0));
        const float neighbor_weight = static_cast<float>(
            2.0 / (dn * (dn + 5.0)));
        const float face_weight = static_cast<float>(
            4.0 / (dn * (dn + 5.0)));
        const Vec3 vertex_term = own * vertex_weight;
        const Vec3 neighbor_term =
            smoothed_neighbor_sums[vertex] * neighbor_weight;
        const Vec3 face_term =
            smoothed_face_point_sums[vertex] * face_weight;
        result.positions[vertex] = vertex_term + neighbor_term + face_term;
    }
    return result;
}

std::vector<Vec3> smooth_vertex_normals(const LoadedMesh &mesh,
                                        bool reference) {
    std::vector<Vec3> result(mesh.positions.size());
    const std::vector<std::size_t> offsets = face_offsets(mesh);
    for (std::size_t face = 0u; face < mesh.face_counts.size(); ++face) {
        const std::size_t count = static_cast<std::size_t>(mesh.face_counts[face]);
        const std::size_t offset = offsets[face];
        const auto sample_position = [&](std::size_t corner) {
            const std::int32_t raw = mesh.face_indices[offset + corner];
            if (raw < 0) { fail("mesh contains a negative vertex index"); }
            const std::uint32_t index = static_cast<std::uint32_t>(raw);
            return reference ? reference_position(mesh, index)
                             : position(mesh, index);
        };
        const Vec3 first = sample_position(0u);
        for (std::size_t corner = 1u; corner + 1u < count; ++corner) {
            const Vec3 normal = cross(
                sample_position(corner) - first,
                sample_position(corner + 1u) - first);
            const std::uint32_t i0 = static_cast<std::uint32_t>(
                mesh.face_indices[offset]);
            const std::uint32_t i1 = static_cast<std::uint32_t>(
                mesh.face_indices[offset + corner]);
            const std::uint32_t i2 = static_cast<std::uint32_t>(
                mesh.face_indices[offset + corner + 1u]);
            result[i0] = result[i0] + normal;
            result[i1] = result[i1] + normal;
            result[i2] = result[i2] + normal;
        }
    }
    for (Vec3 &normal : result) {
        if (!(length_squared(normal) > 0.0f)) {
            fail("mesh contains a vertex without a valid normal");
        }
        normal = normalize(normal);
    }
    return result;
}

std::uint32_t vertex_index(const LoadedMesh &mesh, std::size_t index,
                           std::uint32_t vertex_base) {
    if (index >= mesh.face_indices.size() || mesh.face_indices[index] < 0) {
        fail("mesh contains an invalid face vertex index");
    }
    const std::uint64_t value = static_cast<std::uint64_t>(vertex_base) +
        static_cast<std::uint32_t>(mesh.face_indices[index]);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        fail("combined vertex index exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

ClassicAlembicAssetInput build_xgen_classic_alembic_asset_input_impl(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicLimits &limits,
    const ArchiveSampleSelection &selection) {
    if (description.patches.empty()) {
        fail("description has no patches");
    }
    if (limits.max_objects == 0u || limits.max_vertices == 0u ||
        limits.max_faces == 0u || limits.max_face_vertices == 0u ||
        limits.max_triangles == 0u) {
        fail("limits must be nonzero");
    }
    AbcFactory::IFactory factory;
    const Abc::IArchive archive = factory.getArchive(archive_path.string());
    if (!archive.valid()) { fail("cannot open archive: " + archive_path.string()); }

    ClassicAlembicAssetInput result{};
    const auto reference_surface =
        std::make_shared<ImportedReferenceSurface>();
    result.reference_surface = reference_surface;
    float guide_cage_root_squared_distance_sum = 0.0f;
    std::size_t subdivision_guide_count = 0u;
    for (const ClassicPatch &patch : description.patches) {
        const LoadedMesh mesh = load_patch_mesh(
            archive, patch.name, limits, selection);
        result.source_vertex_count += mesh.positions.size();
        result.source_face_count += mesh.face_counts.size();
        const std::vector<std::size_t> offsets = face_offsets(mesh);
        const bool subdivide = patch.type == "Subd";
        std::vector<std::vector<std::uint32_t>> vertex_faces(
            mesh.positions.size());
        for (std::uint32_t face_id = 0u;
             face_id < mesh.face_counts.size(); ++face_id) {
            const std::size_t offset = offsets[face_id];
            const std::size_t count = static_cast<std::size_t>(
                mesh.face_counts[face_id]);
            for (std::size_t corner = 0u; corner < count; ++corner) {
                const std::int32_t raw = mesh.face_indices[offset + corner];
                if (raw < 0 ||
                    static_cast<std::size_t>(raw) >= mesh.positions.size()) {
                    fail("mesh contains an invalid face vertex index");
                }
                vertex_faces[static_cast<std::uint32_t>(raw)].push_back(
                    face_id);
            }
        }
        const LimitVertexApproximation limit_vertices = subdivide
            ? xgen_limit_vertices(mesh)
            : LimitVertexApproximation{};
        const std::vector<PtexFaceLocation> ptex_faces = subdivide
            ? build_ptex_face_locations(mesh)
            : std::vector<PtexFaceLocation>{};
        if (limits.subd_face_resolution == 0u ||
            limits.subd_face_resolution > 64u) {
            fail("subdivision face resolution must be in [1, 64]");
        }
        std::unique_ptr<SubdEvaluator> subd;
        if (subdivide) {
            result.subdivision_face_count += patch.face_ids.size();
            for (const std::uint32_t face_id : patch.face_ids) {
                if (face_id >= ptex_faces.size()) {
                    fail("patch PTEX face ID is outside the Alembic mesh: " +
                         std::to_string(face_id));
                }
            }
            subd = std::make_unique<SubdEvaluator>(mesh, patch.face_ids);
        }
        auto append_vertex = [&](Vec3 value, Vec3 normal,
                                 Vec3 reference_value,
                                 Vec3 reference_normal) -> std::uint32_t {
            if (result.asset.positions.size() >= limits.max_vertices ||
                result.asset.positions.size() >=
                    std::numeric_limits<std::uint32_t>::max()) {
                fail("combined vertex count exceeds the limit");
            }
            const std::uint32_t index =
                static_cast<std::uint32_t>(result.asset.positions.size());
            result.asset.positions.push_back(value);
            result.asset.normals.push_back(normal);
            result.asset.reference_positions.push_back(reference_value);
            result.asset.reference_normals.push_back(reference_normal);
            return index;
        };
        std::uint32_t vertex_base = 0u;
        if (!subdivide) {
            if (mesh.positions.size() >
                limits.max_vertices - result.asset.positions.size() ||
                mesh.positions.size() >
                    std::numeric_limits<std::uint32_t>::max() -
                        result.asset.positions.size()) {
                fail("combined vertex count exceeds the limit");
            }
            vertex_base = static_cast<std::uint32_t>(
                result.asset.positions.size());
            result.asset.positions.reserve(
                result.asset.positions.size() + mesh.positions.size());
            const std::vector<Vec3> current_normals =
                smooth_vertex_normals(mesh, false);
            const std::vector<Vec3> reference_normals =
                smooth_vertex_normals(mesh, true);
            for (std::uint32_t index = 0u; index < mesh.positions.size(); ++index) {
                append_vertex(position(mesh, index), current_normals[index],
                              reference_position(mesh, index),
                              reference_normals[index]);
            }
        }

        std::unordered_map<std::uint32_t, FaceRange> selected;
        selected.reserve(patch.face_ids.size());
        for (const std::uint32_t face_id : patch.face_ids) {
            const std::uint32_t coarse_face = subdivide
                ? ptex_faces.at(face_id).coarse_face
                : face_id;
            if (coarse_face >= mesh.face_counts.size()) {
                fail("patch face ID is outside the Alembic mesh: " +
                     std::to_string(face_id));
            }
            const std::size_t offset = offsets[coarse_face];
            const std::size_t count = static_cast<std::size_t>(
                mesh.face_counts[coarse_face]);
            const std::size_t triangle_count = subdivide
                ? static_cast<std::size_t>(limits.subd_face_resolution) *
                      limits.subd_face_resolution * 2u
                : count - 2u;
            if (triangle_count > limits.max_triangles - result.asset.triangles.size()) {
                fail("triangle count exceeds the limit");
            }
            if (triangle_count >
                std::numeric_limits<std::uint32_t>::max() -
                    result.asset.triangles.size()) {
                fail("triangle count exceeds uint32");
            }
            const std::uint32_t first_triangle =
                static_cast<std::uint32_t>(result.asset.triangles.size());
            const std::uint32_t first_vertex = subdivide
                ? static_cast<std::uint32_t>(result.asset.positions.size())
                : vertex_base;
            if (subdivide) {
                const std::uint32_t resolution = limits.subd_face_resolution;
                for (std::uint32_t y = 0u; y <= resolution; ++y) {
                    for (std::uint32_t x = 0u; x <= resolution; ++x) {
                        const float u = static_cast<float>(x) /
                                        static_cast<float>(resolution);
                        const float v = static_cast<float>(y) /
                                        static_cast<float>(resolution);
                        const float surface_v = 1.0f - v;
                        const SubdSample current =
                            subd->evaluate(face_id, u, surface_v);
                        const SubdSample reference =
                            subd->evaluate_reference(face_id, u, surface_v);
                        const SurfaceFrame current_frame =
                            subd_surface_frame(
                                *subd, face_id, u, surface_v, false, current);
                        const SurfaceFrame reference_frame =
                            subd_surface_frame(
                                *subd, face_id, u, surface_v, true, reference);
                        append_vertex(
                            current.position, current_frame.normal,
                            reference.position, reference_frame.normal);
                    }
                }
                const std::uint32_t row = resolution + 1u;
                for (std::uint32_t y = 0u; y < resolution; ++y) {
                    for (std::uint32_t x = 0u; x < resolution; ++x) {
                        const std::uint32_t a = first_vertex + y * row + x;
                        const std::uint32_t b = a + 1u;
                        const std::uint32_t d = a + row;
                        const std::uint32_t c = d + 1u;
                        result.asset.triangles.push_back({a, b, c});
                        result.asset.triangles.push_back({a, c, d});
                    }
                }
            } else {
                const std::uint32_t first =
                    vertex_index(mesh, offset, vertex_base);
                for (std::size_t corner = 1u; corner + 1u < count; ++corner) {
                    result.asset.triangles.push_back({
                        first,
                        vertex_index(mesh, offset + corner, vertex_base),
                        vertex_index(mesh, offset + corner + 1u, vertex_base)});
                }
            }
            double surface_area = 0.0;
            double center_u_length = 0.0;
            double center_v_length = 0.0;
            if (subdivide) {
                Double3 cage[4u]{};
                bool evaluate_corners = count != 4u;
                if (!evaluate_corners) {
                    for (std::size_t corner = 0u; corner < 4u; ++corner) {
                        const std::int32_t source_index =
                            mesh.face_indices[offset + corner];
                        if (source_index < 0) {
                            fail("selected face has a negative vertex index");
                        }
                        const std::uint32_t vertex =
                            static_cast<std::uint32_t>(source_index);
                        evaluate_corners = evaluate_corners ||
                            !limit_vertices.supported_neighborhood[vertex];
                        cage[corner] =
                            to_double(limit_vertices.positions[vertex]);
                    }
                }
                if (evaluate_corners) {
                    cage[0] = to_double(subd->evaluate_reference(
                        face_id, 0.0f, 0.0f).position);
                    cage[1] = to_double(subd->evaluate_reference(
                        face_id, 1.0f, 0.0f).position);
                    cage[2] = to_double(subd->evaluate_reference(
                        face_id, 1.0f, 1.0f).position);
                    cage[3] = to_double(subd->evaluate_reference(
                        face_id, 0.0f, 1.0f).position);
                }
                surface_area = xgen_quad_area(cage);
                center_u_length = xgen_quad_length_u(cage);
                center_v_length = xgen_quad_length_v(cage);
            } else {
                for (std::uint32_t triangle_index = first_triangle;
                     triangle_index < result.asset.triangles.size();
                     ++triangle_index) {
                    const UInt3 triangle = result.asset.triangles[triangle_index];
                    surface_area += 0.5 * triangle_double_area(
                        to_double(result.asset.positions[triangle.x]),
                        to_double(result.asset.positions[triangle.y]),
                        to_double(result.asset.positions[triangle.z]));
                }
                center_u_length = 1.0;
                center_v_length = 1.0;
            }
            if (!std::isfinite(surface_area) || surface_area <= 0.0f) {
                fail("selected face has invalid surface area");
            }
            if (!std::isfinite(center_u_length) ||
                !std::isfinite(center_v_length) ||
                center_u_length <= 0.0 || center_v_length <= 0.0) {
                fail("selected face has invalid surface-compensation lengths");
            }
            Vec3 reference_bounds_min{
                std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity()};
            Vec3 reference_bounds_max{
                -std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity()};
            const auto include_reference_vertex = [&](std::uint32_t vertex) {
                const Vec3 value = reference_position(mesh, vertex);
                reference_bounds_min.x = std::min(reference_bounds_min.x, value.x);
                reference_bounds_min.y = std::min(reference_bounds_min.y, value.y);
                reference_bounds_min.z = std::min(reference_bounds_min.z, value.z);
                reference_bounds_max.x = std::max(reference_bounds_max.x, value.x);
                reference_bounds_max.y = std::max(reference_bounds_max.y, value.y);
                reference_bounds_max.z = std::max(reference_bounds_max.z, value.z);
            };
            if (subdivide) {
                // SESubdImpl::faceBoundingBox starts with the active cage face,
                // then visits getFaceUmbrella for every corner and includes all
                // vertices of every incident face.
                for (std::size_t corner = 0u; corner < count; ++corner) {
                    const std::uint32_t vertex = static_cast<std::uint32_t>(
                        mesh.face_indices[offset + corner]);
                    for (const std::uint32_t incident_face :
                         vertex_faces[vertex]) {
                        const std::size_t incident_offset =
                            offsets[incident_face];
                        const std::size_t incident_count =
                            static_cast<std::size_t>(
                                mesh.face_counts[incident_face]);
                        for (std::size_t incident_corner = 0u;
                             incident_corner < incident_count;
                             ++incident_corner) {
                            include_reference_vertex(
                                static_cast<std::uint32_t>(mesh.face_indices[
                                    incident_offset + incident_corner]));
                        }
                    }
                }
            } else {
                for (std::size_t corner = 0u; corner < count; ++corner) {
                    include_reference_vertex(static_cast<std::uint32_t>(
                        mesh.face_indices[offset + corner]));
                }
            }
            if (!std::isfinite(reference_bounds_min.x) ||
                !std::isfinite(reference_bounds_min.y) ||
                !std::isfinite(reference_bounds_min.z) ||
                !std::isfinite(reference_bounds_max.x) ||
                !std::isfinite(reference_bounds_max.y) ||
                !std::isfinite(reference_bounds_max.z) ||
                reference_bounds_min.x > reference_bounds_max.x ||
                reference_bounds_min.y > reference_bounds_max.y ||
                reference_bounds_min.z > reference_bounds_max.z) {
                fail("selected face has invalid reference bounds");
            }
            result.surface_faces.push_back({
                patch.name, face_id, first_triangle,
                static_cast<std::uint32_t>(triangle_count),
                subdivide ? limits.subd_face_resolution : 0u, surface_area,
                center_u_length, center_v_length, reference_bounds_min,
                reference_bounds_max});
            selected.emplace(face_id, FaceRange{
                offset, count, first_triangle, first_vertex});
            ++result.selected_face_count;
        }

        for (const ClassicGuide &guide : patch.guides) {
            const auto found = selected.find(guide.face_id);
            if (found == selected.end()) {
                fail("guide face was not selected");
            }
            const FaceRange face = found->second;
            if (!subdivide && face.count != 4u) {
                fail("guide root evaluation currently requires quad patch faces");
            }
            const float u = static_cast<float>(guide.patch_u);
            const float v = static_cast<float>(guide.patch_v);
            const float surface_v = 1.0f - v;
            if (!std::isfinite(u) || !std::isfinite(v) ||
                u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
                fail("guide patch coordinates must be finite and in [0, 1]");
            }
            Vec3 root{};
            Vec3 du{};
            Vec3 dv{};
            Vec3 reference_root{};
            Vec3 reference_du{};
            Vec3 reference_dv{};
            Vec3 cage_root{};
            Vec3 reference_cage_root{};
            if (subdivide) {
                const SubdSample sample =
                    subd->evaluate(guide.face_id, u, surface_v);
                const SubdSample reference_sample =
                    subd->evaluate_reference(guide.face_id, u, surface_v);
                root = sample.position;
                du = sample.du;
                dv = sample.dv;
                reference_root = reference_sample.position;
                reference_du = reference_sample.du;
                reference_dv = reference_sample.dv;
                cage_root = root;
                reference_cage_root = reference_root;
                if (face.count == 4u) {
                    Vec3 corners[4u]{};
                    Vec3 reference_corners[4u]{};
                    for (std::size_t corner = 0u; corner < 4u; ++corner) {
                        const std::int32_t source_index =
                            mesh.face_indices[face.offset + corner];
                        if (source_index < 0) {
                            fail("guide face has a negative vertex index");
                        }
                        const std::uint32_t vertex =
                            static_cast<std::uint32_t>(source_index);
                        corners[corner] = position(mesh, vertex);
                        reference_corners[corner] =
                            reference_position(mesh, vertex);
                    }
                    cage_root =
                        corners[0] * ((1.0f - u) * (1.0f - surface_v)) +
                        corners[1] * (u * (1.0f - surface_v)) +
                        corners[2] * (u * surface_v) +
                        corners[3] * ((1.0f - u) * surface_v);
                    reference_cage_root =
                        reference_corners[0] *
                            ((1.0f - u) * (1.0f - surface_v)) +
                        reference_corners[1] * (u * (1.0f - surface_v)) +
                        reference_corners[2] * (u * surface_v) +
                        reference_corners[3] * ((1.0f - u) * surface_v);
                }
                const float distance_squared = length_squared(root - cage_root);
                guide_cage_root_squared_distance_sum += distance_squared;
                result.guide_cage_root_max_distance = std::max(
                    result.guide_cage_root_max_distance,
                    std::sqrt(distance_squared));
                ++subdivision_guide_count;
            } else {
                Vec3 corners[4u]{};
                Vec3 reference_corners[4u]{};
                for (std::size_t corner = 0u; corner < 4u; ++corner) {
                    const std::int32_t source_index =
                        mesh.face_indices[face.offset + corner];
                    if (source_index < 0) {
                        fail("guide face has a negative vertex index");
                    }
                    const std::uint32_t vertex =
                        static_cast<std::uint32_t>(source_index);
                    corners[corner] = position(mesh, vertex);
                    reference_corners[corner] =
                        reference_position(mesh, vertex);
                }
                cage_root =
                    corners[0] * ((1.0f - u) * (1.0f - surface_v)) +
                    corners[1] * (u * (1.0f - surface_v)) +
                    corners[2] * (u * surface_v) +
                    corners[3] * ((1.0f - u) * surface_v);
                reference_cage_root =
                    reference_corners[0] *
                        ((1.0f - u) * (1.0f - surface_v)) +
                    reference_corners[1] * (u * (1.0f - surface_v)) +
                    reference_corners[2] * (u * surface_v) +
                    reference_corners[3] * ((1.0f - u) * surface_v);
                root = cage_root;
                du = (corners[1] - corners[0]) * (1.0f - surface_v) +
                     (corners[2] - corners[3]) * surface_v;
                dv = (corners[3] - corners[0]) * (1.0f - u) +
                     (corners[2] - corners[1]) * u;
                reference_root = reference_cage_root;
                reference_du =
                    (reference_corners[1] - reference_corners[0]) *
                        (1.0f - surface_v) +
                    (reference_corners[2] - reference_corners[3]) * surface_v;
                reference_dv =
                    (reference_corners[3] - reference_corners[0]) * (1.0f - u) +
                    (reference_corners[2] - reference_corners[1]) * u;
            }
            GuideInput output{};
            const SurfaceFrame current_frame = subdivide
                ? subd_surface_frame(
                      *subd, guide.face_id, u, surface_v, false,
                      {root, du, dv})
                : xgen_surface_frame(du, dv);
            const SurfaceFrame reference_frame = subdivide
                ? subd_surface_frame(
                      *subd, guide.face_id, u, surface_v, true,
                      {reference_root, reference_du, reference_dv})
                : xgen_surface_frame(reference_du, reference_dv);
            output.root_normal = current_frame.normal;
            output.root_uv = {u, v};
            output.surface_face_id = guide.face_id;
            output.reference_root_position = reference_root;
            output.reference_root_normal = reference_frame.normal;
            output.reference_root_tangent = reference_frame.tangent;
            output.reference_root_binormal = reference_frame.binormal;
            if (((guide.interpolation_count & 1u) == 0u &&
                 guide.interpolation_count != 0u) ||
                guide.interpolation_offset > patch.guide_interpolation.size() ||
                guide.interpolation_count > patch.guide_interpolation.size() -
                                                guide.interpolation_offset) {
                fail("guide interpolation payload is invalid");
            }
            if (guide.interpolation_count != 0u) {
                const auto interpolation = std::span{
                    patch.guide_interpolation}.subspan(
                        guide.interpolation_offset, guide.interpolation_count);
                const auto checked_float = [](double value) {
                    const float converted = static_cast<float>(value);
                    if (!std::isfinite(value) || !std::isfinite(converted)) {
                        fail("guide interpolation contains a non-finite value");
                    }
                    return converted;
                };
                output.support_radii.reserve((interpolation.size() + 1u) / 2u);
                output.support_angles.reserve(interpolation.size() / 2u);
                // XgGuide::setInterpolation applies max(1 + blend, 1) to
                // every authored support radius. Keep the source/parse side
                // in double, then narrow the already-scaled value at the
                // float-only runtime boundary.
                const double radius_scale = std::max(1.0 + guide.blend, 1.0);
                output.support_radii.push_back(
                    checked_float(interpolation[0u] * radius_scale));
                for (std::size_t index = 1u; index < interpolation.size(); index += 2u) {
                    output.support_radii.push_back(
                        checked_float(interpolation[index] * radius_scale));
                    output.support_angles.push_back(checked_float(interpolation[index + 1u]));
                }
                output.support_radius = output.support_radii.front();
            }
            float local_u = u;
            float local_v = v;
            std::uint32_t triangle = face.first_triangle;
            if (subdivide) {
                const std::uint32_t resolution = limits.subd_face_resolution;
                const float scaled_u = u * static_cast<float>(resolution);
                const float scaled_v = v * static_cast<float>(resolution);
                const std::uint32_t cell_u = std::min(
                    static_cast<std::uint32_t>(scaled_u), resolution - 1u);
                const std::uint32_t cell_v = std::min(
                    static_cast<std::uint32_t>(scaled_v), resolution - 1u);
                local_u = scaled_u - static_cast<float>(cell_u);
                local_v = scaled_v - static_cast<float>(cell_v);
                triangle += (cell_v * resolution + cell_u) * 2u;
            }
            if (local_u >= local_v) {
                output.triangle_index = triangle;
                output.barycentric = {local_u - local_v, local_v};
            } else {
                output.triangle_index = triangle + 1u;
                output.barycentric = {local_u, local_v - local_u};
            }
            output.cvs.reserve(guide.cv_count);
            for (std::size_t index = 0u; index < guide.cv_count; ++index) {
                if (guide.cv_offset + index >= patch.guide_cvs.size()) {
                    fail("guide CV range is invalid");
                }
                const ClassicFloat3 local =
                    patch.guide_cvs[guide.cv_offset + index];
                const Vec3 local_float{
                    static_cast<float>(local.x),
                    static_cast<float>(local.y),
                    static_cast<float>(local.z)};
                if (!std::isfinite(local.x) || !std::isfinite(local.y) ||
                    !std::isfinite(local.z) ||
                    !std::isfinite(local_float.x) ||
                    !std::isfinite(local_float.y) ||
                    !std::isfinite(local_float.z)) {
                    fail("guide CV cannot be represented as float");
                }
                // Classic guide CVs are authored in the guide's patch frame,
                // not as world-space displacement vectors. This mirrors
                // XgBasePrimitive::transformGuidesToSurface(): local x is cU,
                // local y is cN, and local z is the frame binormal.
                output.cvs.push_back(
                    root + current_frame.tangent * local_float.x +
                    current_frame.normal * local_float.y +
                    current_frame.binormal * local_float.z);
            }
            result.asset.guides.emplace_back(std::move(output));
        }
        if (subd) {
            reference_surface->add(patch.name, std::move(subd));
        }
    }
    if (subdivision_guide_count != 0u) {
        result.guide_cage_root_rms_distance = std::sqrt(
            guide_cage_root_squared_distance_sum /
            static_cast<float>(subdivision_guide_count));
    }
    return result;
}

ClassicAlembicAssetInput build_xgen_classic_alembic_asset_input(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicLimits &limits) {
    return build_xgen_classic_alembic_asset_input_impl(
        description, archive_path, limits, {});
}

ClassicAlembicAssetInput build_xgen_classic_alembic_asset_input(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicFrameSample &sample,
    const ClassicAlembicLimits &limits) {
    if (!std::isfinite(sample.frame) ||
        !std::isfinite(sample.lookup_offset) ||
        !std::isfinite(sample.frames_per_second) ||
        !(sample.frames_per_second > 0.0)) {
        fail("motion frame, lookup offset, and FPS must be finite with positive FPS");
    }
    const double time_seconds =
        (sample.frame + sample.lookup_offset) /
        sample.frames_per_second;
    if (!std::isfinite(time_seconds)) {
        fail("motion lookup time is not finite");
    }
    return build_xgen_classic_alembic_asset_input_impl(
        description, archive_path, limits,
        {false, time_seconds, sample.interpolation});
}

void apply_xgen_classic_alembic_guide_cache_impl(
    ClassicAlembicAssetInput &asset,
    const std::filesystem::path &cache_path,
    const ClassicAlembicLimits &limits,
    const ArchiveSampleSelection &selection,
    std::string_view wire_names) {
    if (limits.max_objects == 0u || limits.max_vertices == 0u ||
        limits.max_faces == 0u) {
        fail("guide-cache limits must be nonzero");
    }
    AbcFactory::IFactory factory;
    const Abc::IArchive archive = factory.getArchive(cache_path.string());
    if (!archive.valid()) {
        fail("cannot open guide cache: " + cache_path.string());
    }
    std::vector<std::string> curve_names;
    std::vector<std::vector<Vec3>> curves =
        load_guide_cache_curves(
            archive, limits, selection, curve_names);
    if (curves.size() != asset.asset.guides.size()) {
        fail(
            "guide-cache curve count " + std::to_string(curves.size()) +
            " does not match description guide count " +
            std::to_string(asset.asset.guides.size()));
    }
    for (std::size_t index = 0u; index < curves.size(); ++index) {
        if (curves[index].empty()) {
            fail("guide-cache curve has no CVs");
        }
    }

    const auto dot3 = [](Vec3 a, Vec3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    const auto cross3 = [](Vec3 a, Vec3 b) {
        return Vec3{a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x};
    };
    const auto negate3 = [](Vec3 value) {
        return Vec3{-value.x, -value.y, -value.z};
    };
    const auto directional_radius = [&](const GuideInput &guide,
                                        Vec3 tangent, Vec3 offset) {
        const float normal_projection =
            dot3(offset, guide.reference_root_normal);
        Vec3 projected{
            offset.x - guide.reference_root_normal.x * normal_projection,
            offset.y - guide.reference_root_normal.y * normal_projection,
            offset.z - guide.reference_root_normal.z * normal_projection};
        const float projected_squared = dot3(projected, projected);
        if (guide.support_angles.empty() ||
            !(projected_squared > 1.0e-20f)) {
            return guide.support_radii.front();
        }
        const float inverse_length = 1.0f / std::sqrt(projected_squared);
        projected.x *= inverse_length;
        projected.y *= inverse_length;
        projected.z *= inverse_length;
        const float cosine = std::clamp(
            dot3(projected, tangent), -1.0f, 1.0f);
        float angle = 1.0f - cosine * std::abs(cosine);
        if (dot3(cross3(projected, tangent),
                 guide.reference_root_normal) < 0.0f) {
            angle = 4.0f - angle;
        }
        const auto upper = std::upper_bound(
            guide.support_angles.begin(), guide.support_angles.end(), angle);
        std::size_t lower_index{};
        std::size_t upper_index{};
        float lower_angle{};
        float upper_angle{};
        if (upper == guide.support_angles.begin()) {
            lower_index = guide.support_angles.size() - 1u;
            upper_index = 0u;
            lower_angle = guide.support_angles[lower_index] - 4.0f;
            upper_angle = guide.support_angles[upper_index];
        } else if (upper == guide.support_angles.end()) {
            lower_index = guide.support_angles.size() - 1u;
            upper_index = 0u;
            lower_angle = guide.support_angles[lower_index];
            upper_angle = guide.support_angles[upper_index] + 4.0f;
        } else {
            upper_index = static_cast<std::size_t>(
                upper - guide.support_angles.begin());
            lower_index = upper_index - 1u;
            lower_angle = guide.support_angles[lower_index];
            upper_angle = guide.support_angles[upper_index];
        }
        const float denominator = upper_angle - lower_angle;
        if (!(denominator > 0.0f)) {
            return guide.support_radii[upper_index + 1u];
        }
        const float blend = std::clamp(
            (angle - lower_angle) / denominator, 0.0f, 1.0f);
        return guide.support_radii[lower_index + 1u] * (1.0f - blend) +
               guide.support_radii[upper_index + 1u] * blend;
    };
    for (std::size_t index = 0u; index < asset.asset.guides.size(); ++index) {
        GuideInput &guide = asset.asset.guides[index];
        if (guide.support_radii.size() != guide.support_angles.size() + 1u ||
            guide.support_angles.empty()) {
            continue;
        }
        const Vec3 tangent = guide.reference_root_tangent;
        const Vec3 binormal = guide.reference_root_binormal;
        const Vec3 candidates[4u]{
            tangent, binormal, negate3(tangent), negate3(binormal)};
        double best_score = std::numeric_limits<double>::infinity();
        std::size_t best{};
        for (std::size_t rotation = 0u; rotation < 4u; ++rotation) {
            double score{};
            for (std::size_t other = 0u;
                 other < asset.asset.guides.size(); ++other) {
                if (other == index) { continue; }
                const GuideInput &neighbor = asset.asset.guides[other];
                if (!(dot3(neighbor.reference_root_normal,
                           guide.reference_root_normal) >= 0.0f)) {
                    continue;
                }
                const Vec3 offset{
                    neighbor.reference_root_position.x -
                        guide.reference_root_position.x,
                    neighbor.reference_root_position.y -
                        guide.reference_root_position.y,
                    neighbor.reference_root_position.z -
                        guide.reference_root_position.z};
                const float distance_squared = dot3(offset, offset);
                const float broad_radius = guide.support_radii.front();
                if (!(distance_squared < broad_radius * broad_radius)) {
                    continue;
                }
                const float radius = directional_radius(
                    guide, candidates[rotation], offset);
                const float distance = std::sqrt(distance_squared);
                if (radius > distance && radius > 0.0f) {
                    const double penetration =
                        static_cast<double>(radius - distance) / radius;
                    score += penetration * penetration;
                }
            }
            if (score < best_score) {
                best_score = score;
                best = rotation;
            }
        }
        guide.reference_root_tangent = candidates[best];
        guide.reference_root_binormal =
            best == 0u ? binormal :
            best == 1u ? negate3(tangent) :
            best == 2u ? negate3(binormal) : tangent;
    }

    const std::size_t count = curves.size();
    std::vector<std::vector<double>> guide_fingerprints(count);
    std::vector<std::vector<double>> curve_fingerprints(count);
    double guide_scale{};
    double curve_scale{};
    const auto distance = [](Vec3 a, Vec3 b) {
        const double x = static_cast<double>(a.x) - b.x;
        const double y = static_cast<double>(a.y) - b.y;
        const double z = static_cast<double>(a.z) - b.z;
        return std::sqrt(x * x + y * y + z * z);
    };
    for (std::size_t a = 0u; a < count; ++a) {
        if (asset.asset.guides[a].cvs.empty()) {
            fail("description guide has no CVs");
        }
        guide_fingerprints[a].reserve(count - 1u);
        curve_fingerprints[a].reserve(count - 1u);
        for (std::size_t b = 0u; b < count; ++b) {
            if (a == b) { continue; }
            const double guide_distance = distance(
                asset.asset.guides[a].cvs.front(),
                asset.asset.guides[b].cvs.front());
            const double curve_distance = distance(
                curves[a].front(), curves[b].front());
            guide_scale = std::max(guide_scale, guide_distance);
            curve_scale = std::max(curve_scale, curve_distance);
            guide_fingerprints[a].push_back(guide_distance);
            curve_fingerprints[a].push_back(curve_distance);
        }
    }
    if (count > 1u && (!(guide_scale > 0.0) || !(curve_scale > 0.0))) {
        fail("guide-cache root domain is degenerate");
    }
    if (count > 1u) {
        for (auto &values : guide_fingerprints) {
            for (double &value : values) { value /= guide_scale; }
            std::sort(values.begin(), values.end());
        }
        for (auto &values : curve_fingerprints) {
            for (double &value : values) { value /= curve_scale; }
            std::sort(values.begin(), values.end());
        }
    }

    constexpr double incompatible_cost = 1.0e6;
    std::vector<double> costs(count * count, incompatible_cost);
    for (std::size_t guide = 0u; guide < count; ++guide) {
        for (std::size_t curve = 0u; curve < count; ++curve) {
            if (asset.asset.guides[guide].cvs.size() !=
                curves[curve].size()) {
                continue;
            }
            double squared_error{};
            for (std::size_t sample = 0u;
                 sample < guide_fingerprints[guide].size(); ++sample) {
                const double error = guide_fingerprints[guide][sample] -
                    curve_fingerprints[curve][sample];
                squared_error += error * error;
            }
            const double divisor = static_cast<double>(
                std::max<std::size_t>(
                    guide_fingerprints[guide].size(), 1u));
            const double fingerprint_cost =
                std::sqrt(squared_error / divisor);
            const double root_scale = std::max(guide_scale, curve_scale);
            const double root_cost = count <= 1u
                ? 0.0
                : distance(
                      asset.asset.guides[guide].cvs.front(),
                      curves[curve].front()) / root_scale;
            costs[guide * count + curve] =
                root_cost + fingerprint_cost * 1.0e-6;
        }
    }

    // Minimum-cost one-to-one assignment. Dense guide layouts often have
    // locally similar distance fingerprints, where greedy edge selection can
    // consume the correct curve for a later guide.
    std::vector<double> row_potential(count + 1u);
    std::vector<double> column_potential(count + 1u);
    std::vector<std::size_t> column_row(count + 1u);
    std::vector<std::size_t> previous_column(count + 1u);
    for (std::size_t row = 1u; row <= count; ++row) {
        column_row[0u] = row;
        std::size_t column0{};
        std::vector<double> minimum(count + 1u,
                                    std::numeric_limits<double>::infinity());
        std::vector<bool> used(count + 1u);
        do {
            used[column0] = true;
            const std::size_t row0 = column_row[column0];
            double delta = std::numeric_limits<double>::infinity();
            std::size_t column1{};
            for (std::size_t column = 1u; column <= count; ++column) {
                if (used[column]) { continue; }
                const double reduced =
                    costs[(row0 - 1u) * count + column - 1u] -
                    row_potential[row0] - column_potential[column];
                if (reduced < minimum[column]) {
                    minimum[column] = reduced;
                    previous_column[column] = column0;
                }
                if (minimum[column] < delta) {
                    delta = minimum[column];
                    column1 = column;
                }
            }
            if (!std::isfinite(delta)) {
                fail("guide-cache CV counts cannot be matched to description guides");
            }
            for (std::size_t column = 0u; column <= count; ++column) {
                if (used[column]) {
                    row_potential[column_row[column]] += delta;
                    column_potential[column] -= delta;
                } else {
                    minimum[column] -= delta;
                }
            }
            column0 = column1;
        } while (column_row[column0] != 0u);
        do {
            const std::size_t column1 = previous_column[column0];
            column_row[column0] = column_row[column1];
            column0 = column1;
        } while (column0 != 0u);
    }
    std::vector<std::size_t> mapping(count, count);
    for (std::size_t column = 1u; column <= count; ++column) {
        if (column_row[column] != 0u) {
            mapping[column_row[column] - 1u] = column - 1u;
        }
    }
    bool incompatible_mapping =
        std::find(mapping.begin(), mapping.end(), count) != mapping.end();
    for (std::size_t guide = 0u;
         !incompatible_mapping && guide < count; ++guide) {
        incompatible_mapping =
            costs[guide * count + mapping[guide]] >= incompatible_cost;
    }
    if (incompatible_mapping) {
        fail("guide-cache CV counts cannot be matched to description guides");
    }

    const auto trailing_id = [](std::string_view value) {
        std::size_t begin = value.size();
        while (begin != 0u && value[begin - 1u] >= '0' &&
               value[begin - 1u] <= '9') {
            --begin;
        }
        std::uint64_t id{};
        if (begin == value.size()) {
            return std::pair{false, id};
        }
        const auto parsed = std::from_chars(
            value.data() + begin, value.data() + value.size(), id);
        return std::pair{
            parsed.ec == std::errc{} &&
                parsed.ptr == value.data() + value.size(),
            id};
    };
    std::vector<std::uint64_t> wire_ids;
    for (std::size_t begin = 0u; begin <= wire_names.size();) {
        const std::size_t end = wire_names.find(',', begin);
        std::string_view entry = wire_names.substr(
            begin, (end == std::string_view::npos
                        ? wire_names.size() : end) - begin);
        if (!entry.empty()) {
            const auto [valid, id] = trailing_id(entry);
            if (!valid) {
                wire_ids.clear();
                break;
            }
            wire_ids.push_back(id);
        }
        if (end == std::string_view::npos) { break; }
        begin = end + 1u;
    }
    std::vector<std::pair<std::uint64_t, std::size_t>> cache_ids;
    cache_ids.reserve(curve_names.size());
    for (std::size_t curve = 0u; curve < curve_names.size(); ++curve) {
        const auto [valid, id] = trailing_id(curve_names[curve]);
        if (!valid) {
            cache_ids.clear();
            break;
        }
        cache_ids.emplace_back(id, curve);
    }
    bool authored_mapping_applied = false;
    if (wire_ids.size() == count && cache_ids.size() == count) {
        std::vector<std::uint64_t> sorted_wire_ids = wire_ids;
        std::sort(sorted_wire_ids.begin(), sorted_wire_ids.end());
        std::sort(cache_ids.begin(), cache_ids.end());
        const bool unique_wire_ids = std::adjacent_find(
            sorted_wire_ids.begin(), sorted_wire_ids.end()) ==
            sorted_wire_ids.end();
        const bool unique_cache_ids = std::adjacent_find(
            cache_ids.begin(), cache_ids.end(),
            [](const auto &a, const auto &b) {
                return a.first == b.first;
            }) == cache_ids.end();
        const bool matching_ids = std::equal(
            sorted_wire_ids.begin(), sorted_wire_ids.end(),
            cache_ids.begin(), cache_ids.end(),
            [](std::uint64_t wire_id, const auto &cache_id) {
                return wire_id == cache_id.first;
            });
        if (unique_wire_ids && unique_cache_ids && matching_ids) {
            std::vector<std::size_t> authored_mapping(count, count);
            bool compatible = true;
            for (std::size_t guide = 0u; guide < count; ++guide) {
                const auto found = std::lower_bound(
                    sorted_wire_ids.begin(), sorted_wire_ids.end(),
                    wire_ids[guide]);
                const std::size_t rank = static_cast<std::size_t>(
                    found - sorted_wire_ids.begin());
                const std::size_t curve = cache_ids[rank].second;
                authored_mapping[guide] = curve;
                compatible &= asset.asset.guides[guide].cvs.size() ==
                    curves[curve].size();
            }
            if (compatible) {
                // _wireNames is the authored XGen guide order. Cache curves
                // may deform relative to their bound patch, so root-distance
                // heuristics cannot override or reject this association.
                mapping = std::move(authored_mapping);
                authored_mapping_applied = true;
            }
        }
    }

    double pair_squared_error{};
    double pair_max_error{};
    std::size_t pair_count{};
    for (std::size_t a = 0u; a < count; ++a) {
        for (std::size_t b = a + 1u; b < count; ++b) {
            const double guide_distance = distance(
                asset.asset.guides[a].cvs.front(),
                asset.asset.guides[b].cvs.front()) / guide_scale;
            const double curve_distance = distance(
                curves[mapping[a]].front(),
                curves[mapping[b]].front()) / curve_scale;
            const double error = std::abs(guide_distance - curve_distance);
            pair_squared_error += error * error;
            pair_max_error = std::max(pair_max_error, error);
            ++pair_count;
        }
    }
    const double pair_rms_error = pair_count == 0u
        ? 0.0 : std::sqrt(pair_squared_error / pair_count);
    if (!authored_mapping_applied &&
        (pair_rms_error > 1.0e-2 || pair_max_error > 3.0e-2)) {
        fail(
            "guide-cache roots do not match description guides "
            "(normalized pairwise RMS=" +
            std::to_string(pair_rms_error) + ", max=" +
            std::to_string(pair_max_error) + ")");
    }
    for (std::size_t guide = 0u; guide < count; ++guide) {
        std::vector<Vec3> &curve = curves[mapping[guide]];
        // XGen's cache override preserves the guide's evaluated surface root
        // as cGuideGeom[0] while replacing the remaining CVs from the cache.
        curve.front() = asset.asset.guides[guide].cvs.front();
        asset.asset.guides[guide].cvs = std::move(curve);
    }
}

void apply_xgen_classic_alembic_guide_cache(
    ClassicAlembicAssetInput &asset,
    const std::filesystem::path &cache_path,
    const ClassicAlembicLimits &limits) {
    apply_xgen_classic_alembic_guide_cache_impl(
        asset, cache_path, limits, {}, {});
}

void apply_xgen_classic_alembic_guide_cache(
    ClassicAlembicAssetInput &asset,
    const std::filesystem::path &cache_path,
    std::string_view wire_names,
    const ClassicAlembicLimits &limits) {
    apply_xgen_classic_alembic_guide_cache_impl(
        asset, cache_path, limits, {}, wire_names);
}

void apply_xgen_classic_alembic_guide_cache(
    ClassicAlembicAssetInput &asset,
    const std::filesystem::path &cache_path,
    const ClassicAlembicFrameSample &sample,
    const ClassicAlembicLimits &limits) {
    if (!std::isfinite(sample.frame) ||
        !std::isfinite(sample.lookup_offset) ||
        !std::isfinite(sample.frames_per_second) ||
        !(sample.frames_per_second > 0.0)) {
        fail("guide-cache motion frame, lookup offset, and FPS must be finite with positive FPS");
    }
    const double time_seconds =
        (sample.frame + sample.lookup_offset) / sample.frames_per_second;
    if (!std::isfinite(time_seconds)) {
        fail("guide-cache motion lookup time is not finite");
    }
    apply_xgen_classic_alembic_guide_cache_impl(
        asset, cache_path, limits,
        {false, time_seconds, sample.interpolation}, {});
}

void apply_xgen_classic_alembic_guide_cache(
    ClassicAlembicAssetInput &asset,
    const std::filesystem::path &cache_path,
    const ClassicAlembicFrameSample &sample,
    std::string_view wire_names,
    const ClassicAlembicLimits &limits) {
    if (!std::isfinite(sample.frame) ||
        !std::isfinite(sample.lookup_offset) ||
        !std::isfinite(sample.frames_per_second) ||
        !(sample.frames_per_second > 0.0)) {
        fail("guide-cache motion frame, lookup offset, and FPS must be finite with positive FPS");
    }
    const double time_seconds =
        (sample.frame + sample.lookup_offset) / sample.frames_per_second;
    if (!std::isfinite(time_seconds)) {
        fail("guide-cache motion lookup time is not finite");
    }
    apply_xgen_classic_alembic_guide_cache_impl(
        asset, cache_path, limits,
        {false, time_seconds, sample.interpolation}, wire_names);
}

bool xgen_classic_alembic_guide_cache_is_static(
    const std::filesystem::path &cache_path,
    const ClassicAlembicLimits &limits) {
    if (limits.max_objects == 0u) {
        fail("guide-cache object limit must be nonzero");
    }
    AbcFactory::IFactory factory;
    const Abc::IArchive archive = factory.getArchive(cache_path.string());
    if (!archive.valid()) {
        fail("cannot open guide cache: " + cache_path.string());
    }
    std::size_t visited{};
    std::size_t matches{};
    bool result = true;
    inspect_static_curves(
        archive.getTop(), false, visited, limits.max_objects,
        matches, result);
    if (matches == 0u) {
        fail("guide cache contains no ICurves objects");
    }
    return result;
}

bool xgen_classic_alembic_deformation_is_static(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicLimits &limits) {
    if (description.patches.empty()) {
        fail("description has no patches");
    }
    if (limits.max_objects == 0u) {
        fail("object limit must be nonzero");
    }
    AbcFactory::IFactory factory;
    const Abc::IArchive archive = factory.getArchive(archive_path.string());
    if (!archive.valid()) {
        fail("cannot open archive: " + archive_path.string());
    }
    bool result = true;
    for (const ClassicPatch &patch : description.patches) {
        std::size_t visited{};
        std::size_t matches{};
        inspect_static_deformation(
            archive.getTop(), patch.name, false, false, visited,
            limits.max_objects, matches, result);
        if (matches == 0u) {
            fail("patch object not found: " + patch.name);
        }
        if (matches != 1u) {
            fail("patch object resolves to multiple meshes: " + patch.name);
        }
    }
    return result;
}

} // namespace nanoxgen
