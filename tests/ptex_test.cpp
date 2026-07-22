#include "nanoxgen/xgen_ptex.h"

#include <Ptexture.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

bool near(float lhs, float rhs) { return std::abs(lhs - rhs) <= 1.0e-6f; }

struct TemporaryPtex {
    std::filesystem::path path;
    ~TemporaryPtex() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

TemporaryPtex write_ptex() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryPtex result{
        std::filesystem::temp_directory_path() /
        ("nanoxgen-ptex-" + std::to_string(stamp) + ".ptx")};
    Ptex::String error;
    Ptex::PtexPtr<Ptex::PtexWriter> writer{Ptex::PtexWriter::open(
        result.path.c_str(), Ptex::mt_quad, Ptex::dt_float, 1, -1, 2, error,
        false)};
    if (!writer) { throw std::runtime_error(error.c_str()); }
    const float pixels[]{0.0f, 1.0f, 2.0f, 3.0f};
    require(writer->writeFace(0, Ptex::FaceInfo{Ptex::Res{1, 1}}, pixels),
            "cannot write varying PTEX face");
    const float constant = 0.25f;
    require(writer->writeConstantFace(
                1, Ptex::FaceInfo{Ptex::Res{2, 2}}, &constant),
            "cannot write constant PTEX face");
    require(writer->close(error), "cannot close PTEX fixture");
    return result;
}

void test_read_and_filter() {
    const TemporaryPtex fixture = write_ptex();
    nanoxgen::XgenPtexMap map{fixture.path};
    const nanoxgen::XgenPtexInfo &info = map.info();
    require(info.mesh_type == nanoxgen::XgenPtexMeshType::Quad,
            "mesh type mismatch");
    require(info.data_type == nanoxgen::XgenPtexDataType::Float,
            "data type mismatch");
    require(info.channel_count == 1u && info.face_count == 2u,
            "PTEX dimensions mismatch");
    const nanoxgen::XgenPtexFaceInfo varying = map.face_info(0u);
    require(varying.u_resolution == 2u && varying.v_resolution == 2u &&
                !varying.constant,
            "varying face metadata mismatch");
    const nanoxgen::XgenPtexFaceInfo constant = map.face_info(1u);
    require(constant.u_resolution == 4u && constant.v_resolution == 4u &&
                constant.constant,
            "constant face metadata mismatch");
    const std::vector<float> texels = map.read_face_channel(0u);
    require(texels == std::vector<float>({0.0f, 1.0f, 2.0f, 3.0f}),
            "face texel order mismatch");
    require(near(map.sample(1u, 0.125f, 0.875f), 0.25f),
            "constant sample mismatch");
    nanoxgen::XgenPtexSampleOptions point{};
    point.filter = nanoxgen::XgenPtexFilter::Point;
    require(near(map.sample(0u, 0.25f, 0.25f, 0u, point), 0.0f),
            "point sample mismatch");
}

void test_rejections() {
    const TemporaryPtex fixture = write_ptex();
    nanoxgen::XgenPtexMap map{fixture.path};
    try {
        (void)map.sample(2u, 0.5f, 0.5f);
    } catch (const std::runtime_error &) {
        try {
            (void)map.sample(0u, std::nanf(""), 0.5f);
        } catch (const std::runtime_error &) { return; }
    }
    throw std::runtime_error("invalid PTEX request was accepted");
}

} // namespace

int main() try {
    test_read_and_filter();
    test_rejections();
    std::cout << "PTEX tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
