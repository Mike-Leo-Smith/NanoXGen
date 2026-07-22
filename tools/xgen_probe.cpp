#include <XGen/XgSplineAPI.h>

#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: nanoxgen_xgen_probe <xgmExportSplineDataInternal output>\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "failed to open " << argv[1] << '\n';
        return 1;
    }
    std::stringstream bytes;
    bytes << input.rdbuf();
    XGenSplineAPI::XgFnSpline splines;
    if (!splines.load(bytes, bytes.str().size(), 0.0f)) {
        std::cerr << "XgFnSpline::load failed\n";
        return 1;
    }
    std::uint64_t batch_count = 0;
    std::uint64_t curve_count = 0;
    std::uint64_t vertex_count = 0;
    for (auto it = splines.iterator(); !it.isDone(); it.next()) {
        ++batch_count;
        curve_count += it.primitiveCount();
        vertex_count += it.vertexCount();
    }
    std::cout << "{\n"
              << "  \"motion_samples\": " << splines.sampleCount() << ",\n"
              << "  \"batches\": " << batch_count << ",\n"
              << "  \"curves\": " << curve_count << ",\n"
              << "  \"vertices\": " << vertex_count << "\n"
              << "}\n";
    return 0;
}
