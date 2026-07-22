#include "nanoxgen/xgen_ptex.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

const char *mesh_name(nanoxgen::XgenPtexMeshType type) {
    return type == nanoxgen::XgenPtexMeshType::Quad ? "quad" : "triangle";
}

const char *data_name(nanoxgen::XgenPtexDataType type) {
    switch (type) {
    case nanoxgen::XgenPtexDataType::UInt8: return "uint8";
    case nanoxgen::XgenPtexDataType::UInt16: return "uint16";
    case nanoxgen::XgenPtexDataType::Half: return "half";
    case nanoxgen::XgenPtexDataType::Float: return "float";
    }
    return "invalid";
}

} // namespace

int main(int argc, char **argv) try {
    if (argc != 2) {
        throw std::runtime_error("usage: nanoxgen_xgen_ptex_inspect MAP.ptx");
    }
    const nanoxgen::XgenPtexMap map{std::filesystem::path{argv[1]}};
    const nanoxgen::XgenPtexInfo &info = map.info();
    std::uint32_t min_u = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t min_v = min_u;
    std::uint32_t max_u{};
    std::uint32_t max_v{};
    std::uint64_t texels{};
    std::uint32_t constant_faces{};
    float minimum = std::numeric_limits<float>::infinity();
    float maximum = -std::numeric_limits<float>::infinity();
    for (std::uint32_t face = 0u; face < info.face_count; ++face) {
        const nanoxgen::XgenPtexFaceInfo face_info = map.face_info(face);
        min_u = std::min(min_u, face_info.u_resolution);
        min_v = std::min(min_v, face_info.v_resolution);
        max_u = std::max(max_u, face_info.u_resolution);
        max_v = std::max(max_v, face_info.v_resolution);
        texels += static_cast<std::uint64_t>(face_info.u_resolution) *
                  face_info.v_resolution;
        constant_faces += face_info.constant ? 1u : 0u;
        const std::vector<float> values = map.read_face_channel(face);
        for (const float value : values) {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
    }
    std::cout << std::setprecision(9) << "{\"mesh\":\""
              << mesh_name(info.mesh_type) << "\",\"data\":\""
              << data_name(info.data_type) << "\",\"channels\":"
              << info.channel_count << ",\"faces\":" << info.face_count
              << ",\"constant_faces\":" << constant_faces
              << ",\"min_resolution\":[" << min_u << ',' << min_v
              << "],\"max_resolution\":[" << max_u << ',' << max_v
              << "],\"texels\":" << texels << ",\"channel0_min\":"
              << minimum << ",\"channel0_max\":" << maximum << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
