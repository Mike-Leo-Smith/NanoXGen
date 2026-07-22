#include "nanoxgen/xgen_classic_alembic.h"

#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

struct LoadedMesh {
    std::vector<Imath::V3f> positions;
    std::vector<std::int32_t> face_counts;
    std::vector<std::int32_t> face_indices;
    Imath::M44d transform{};
};

struct MeshSearch {
    std::string target;
    std::size_t visited{};
    std::size_t max_objects{};
    std::vector<std::pair<Abc::IObject, Imath::M44d>> matches;
};

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error("Classic Alembic import: " + message);
}

bool finite(const Imath::V3d &value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
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
            AbcGeom::XformSample sample;
            xform.getSchema().get(
                sample, Abc::ISampleSelector{AbcCore::index_t{0}});
            transform = sample.getMatrix() * parent_transform;
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
    result.face_counts.assign(counts->get(), counts->get() + counts->size());
    result.face_indices.assign(indices->get(), indices->get() + indices->size());
    result.transform = transform;
    return result;
}

LoadedMesh load_patch_mesh(const Abc::IArchive &archive, std::string_view name,
                           const ClassicAlembicLimits &limits) {
    MeshSearch search{std::string{name}, 0u, limits.max_objects, {}};
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
        AbcGeom::IPolyMeshSchema::Sample sample;
        mesh.getSchema().get(
            sample, Abc::ISampleSelector{AbcCore::index_t{0}});
        return copy_mesh_sample(sample, transform, limits);
    }
    AbcGeom::ISubD mesh{object, Abc::kWrapExisting};
    if (mesh.getSchema().getNumSamples() == 0u) {
        fail("subdivision mesh has no samples: " + object.getFullName());
    }
    AbcGeom::ISubDSchema::Sample sample;
    mesh.getSchema().get(
        sample, Abc::ISampleSelector{AbcCore::index_t{0}});
    return copy_mesh_sample(sample, transform, limits);
}

Vec3 position(const LoadedMesh &mesh, std::uint32_t index) {
    if (index >= mesh.positions.size()) { fail("face vertex index is out of range"); }
    const Imath::V3f &source = mesh.positions[index];
    Imath::V3d transformed;
    mesh.transform.multVecMatrix(
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

Vec3 direction(const LoadedMesh &mesh, const ClassicFloat3 &source) {
    Imath::V3d transformed;
    mesh.transform.multDirMatrix(
        Imath::V3d{source.x, source.y, source.z}, transformed);
    if (!finite(transformed)) { fail("guide contains a non-finite direction"); }
    const Vec3 result{static_cast<float>(transformed.x),
                      static_cast<float>(transformed.y),
                      static_cast<float>(transformed.z)};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z)) {
        fail("guide direction cannot be represented as float");
    }
    return result;
}

struct FaceRange {
    std::size_t offset{};
    std::size_t count{};
    std::uint32_t first_triangle{};
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

ClassicAlembicAssetInput build_xgen_classic_alembic_asset_input(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicLimits &limits) {
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
    for (const ClassicPatch &patch : description.patches) {
        const LoadedMesh mesh = load_patch_mesh(archive, patch.name, limits);
        result.source_vertex_count += mesh.positions.size();
        result.source_face_count += mesh.face_counts.size();
        if (mesh.positions.size() > limits.max_vertices - result.asset.positions.size()) {
            fail("combined vertex count exceeds the limit");
        }
        if (mesh.positions.size() >
            std::numeric_limits<std::uint32_t>::max() -
                result.asset.positions.size()) {
            fail("combined vertex count exceeds uint32");
        }
        const std::uint32_t vertex_base =
            static_cast<std::uint32_t>(result.asset.positions.size());
        result.asset.positions.reserve(
            result.asset.positions.size() + mesh.positions.size());
        for (std::uint32_t index = 0u; index < mesh.positions.size(); ++index) {
            result.asset.positions.push_back(position(mesh, index));
        }

        const std::vector<std::size_t> offsets = face_offsets(mesh);
        std::unordered_map<std::uint32_t, FaceRange> selected;
        selected.reserve(patch.face_ids.size());
        for (const std::uint32_t face_id : patch.face_ids) {
            if (face_id >= mesh.face_counts.size()) {
                fail("patch face ID is outside the Alembic mesh: " +
                     std::to_string(face_id));
            }
            const std::size_t offset = offsets[face_id];
            const std::size_t count = static_cast<std::size_t>(
                mesh.face_counts[face_id]);
            const std::size_t triangle_count = count - 2u;
            if (triangle_count > limits.max_triangles - result.asset.triangles.size()) {
                fail("triangle count exceeds the limit");
            }
            if (result.asset.triangles.size() >
                std::numeric_limits<std::uint32_t>::max()) {
                fail("triangle count exceeds uint32");
            }
            const std::uint32_t first_triangle =
                static_cast<std::uint32_t>(result.asset.triangles.size());
            const std::uint32_t first = vertex_index(mesh, offset, vertex_base);
            for (std::size_t corner = 1u; corner + 1u < count; ++corner) {
                result.asset.triangles.push_back({
                    first,
                    vertex_index(mesh, offset + corner, vertex_base),
                    vertex_index(mesh, offset + corner + 1u, vertex_base)});
            }
            selected.emplace(face_id, FaceRange{offset, count, first_triangle});
            ++result.selected_face_count;
        }

        for (const ClassicGuide &guide : patch.guides) {
            const auto found = selected.find(guide.face_id);
            if (found == selected.end()) {
                fail("guide face was not selected");
            }
            const FaceRange face = found->second;
            if (face.count != 4u) {
                fail("guide root evaluation currently requires quad patch faces");
            }
            const float u = static_cast<float>(guide.patch_u);
            const float v = static_cast<float>(guide.patch_v);
            if (!std::isfinite(u) || !std::isfinite(v) ||
                u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
                fail("guide patch coordinates must be finite and in [0, 1]");
            }
            Vec3 corners[4u]{};
            for (std::size_t corner = 0u; corner < 4u; ++corner) {
                const std::int32_t source_index =
                    mesh.face_indices[face.offset + corner];
                if (source_index < 0) { fail("guide face has a negative vertex index"); }
                corners[corner] = position(
                    mesh, static_cast<std::uint32_t>(source_index));
            }
            const Vec3 root = corners[0] * ((1.0f - u) * (1.0f - v)) +
                              corners[1] * (u * (1.0f - v)) +
                              corners[2] * (u * v) +
                              corners[3] * ((1.0f - u) * v);
            const Vec3 du = (corners[1] - corners[0]) * (1.0f - v) +
                            (corners[2] - corners[3]) * v;
            const Vec3 dv = (corners[3] - corners[0]) * (1.0f - u) +
                            (corners[2] - corners[1]) * u;
            GuideInput output{};
            output.root_normal = normalize(cross(du, dv));
            output.root_uv = {u, v};
            if (u >= v) {
                output.triangle_index = face.first_triangle;
                output.barycentric = {u - v, v};
            } else {
                output.triangle_index = face.first_triangle + 1u;
                output.barycentric = {u, v - u};
            }
            output.cvs.reserve(guide.cv_count);
            for (std::size_t index = 0u; index < guide.cv_count; ++index) {
                if (guide.cv_offset + index >= patch.guide_cvs.size()) {
                    fail("guide CV range is invalid");
                }
                output.cvs.push_back(
                    root + direction(mesh, patch.guide_cvs[guide.cv_offset + index]));
            }
            result.asset.guides.emplace_back(std::move(output));
        }
    }
    return result;
}

} // namespace nanoxgen
