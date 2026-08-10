#include "nanoxgen/xgen_color_expression.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

void require_near(
    nanoxgen::Vec3 actual, nanoxgen::Vec3 expected,
    float tolerance, const char *message) {
    require(
        std::abs(actual.x - expected.x) <= tolerance &&
            std::abs(actual.y - expected.y) <= tolerance &&
            std::abs(actual.z - expected.z) <= tolerance,
        message);
}

} // namespace

int main() {
    try {
        constexpr std::string_view source = R"(
$Scale=2.0;
$Type=0.5;
$Jitter=0.1498;
$noise=voronoi(
    $Pref*10*$Scale,$Type*4+1,$Jitter,2.0,0.2738*9+1,4.0,0.5056);
$colorA=[1,1,0.996078];
$colorB=[0.814815,0.431193,0];
$colorC=[0.988235,0.988235,0.988235];
$mixA=mix($colorA,$colorB,$noise);
$mixB=mix($colorB,$colorC,$noise);
mix($mixA,$mixB,$noise*2)
)";
        const std::vector<nanoxgen::Vec3> positions{
            {0.1f, 0.2f, 0.3f},
            {1.1f, 2.2f, 3.3f},
            {-4.0f, 5.0f, 6.0f}};
        std::vector<nanoxgen::Vec3> values(positions.size());
        nanoxgen::evaluate_xgen_color_expression(
            source, positions, values);
        const nanoxgen::Vec3 oracle[]{
            {1.01419151f, 1.07473636f, 1.14326012f},
            {1.03192544f, 1.13150668f, 1.24387848f},
            {1.01279521f, 1.07026291f, 1.13532984f}};
        for (std::size_t index = 0u; index < values.size(); ++index) {
            require_near(
                values[index], oracle[index], 2e-6f,
                "Autodesk SeExpr color oracle mismatch");
        }

        constexpr std::size_t parallel_count = 131072u;
        std::vector<nanoxgen::Vec3> repeated_positions(parallel_count);
        for (std::size_t index = 0u; index < parallel_count; ++index) {
            repeated_positions[index] = positions[index % positions.size()];
        }
        std::vector<nanoxgen::Vec3> parallel_values(parallel_count);
        nanoxgen::NanoXGenContext context{4u};
        nanoxgen::evaluate_xgen_color_expression(
            source, repeated_positions, parallel_values, &context);
        for (std::size_t index = 0u; index < parallel_count; ++index) {
            require_near(
                parallel_values[index], values[index % values.size()],
                0.0f, "parallel color expression mismatch");
        }

        bool rejected_rand = false;
        try {
            nanoxgen::evaluate_xgen_color_expression(
                "rand()", positions, values);
        } catch (const std::runtime_error &) {
            rejected_rand = true;
        }
        require(
            rejected_rand,
            "rand() without XGen expression seed state was accepted");
        std::cout << "NanoXGen color expression tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
