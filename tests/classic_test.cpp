#include "nanoxgen/xgen_classic.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace nanoxgen;

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

ClassicCollection parse(const std::string &text,
                        const ClassicParseLimits &limits = {}) {
    std::istringstream input(text);
    return parse_xgen_classic_collection(input, limits);
}

void require_fails(const std::string &text, const char *needle,
                   const ClassicParseLimits &limits = {}) {
    try {
        (void)parse(text, limits);
    } catch (const std::exception &error) {
        if (std::string(error.what()).find(needle) == std::string::npos) {
            throw std::runtime_error(
                "expected diagnostic '" + std::string(needle) +
                "', got '" + error.what() + "'");
        }
        return;
    }
    throw std::runtime_error(
        "malformed Classic collection was accepted; expected '" +
        std::string(needle) + "'");
}

std::string fixture(std::string id = "42", std::string loc = ".25 .75 7",
                    std::string cv = "4 5 6") {
    return
        "# fixture\n"
        "FileVersion 18\n\n"
        "Palette\n"
        "\tname\tpalette\n"
        "\texpression\thash($id)\\nmap('${DESC}/density')\n"
        "\tendAttrs\n\n"
        "Description\n"
        "\tname\tfur\n"
        "\tdescriptionId\t2\n"
        "\tendAttrs\n"
        "SplinePrimitive\n"
        "\twidth\thash($id)*0.01+0.01\n"
        "\tendAttrs\n"
        "RandomGenerator\n"
        "\tdensity\t100\n"
        "\tendAttrs\n"
        "\tActive\tSplinePrimitive\n"
        "\tActive\tRandomGenerator\n"
        "Patches fur 1\n"
        "Patch Subd\n"
        "\tname\tpatchShape\n"
        "\tfaceIds\t1 7\n"
        "\tunknown\tpreserved\n"
        "Guides Spline 1\n"
        "\tid\t" + id + "\n"
        "\tloc\t" + loc + "\n"
        "\tblend\t.5\n"
        "\tinterp\t1:2:3\n"
        "\tCVs\t3\n"
        "\t1 2 3\n"
        "\t" + cv + "\n"
        "endObject\n"
        "endPatches\n";
}

void test_typed_parse() {
    const ClassicCollection collection = parse(fixture());
    require(collection.file_version == 18u, "FileVersion mismatch");
    require(collection.descriptions.size() == 1u, "description count mismatch");
    const ClassicAttribute *palette_name = find_classic_attribute(
        collection.palette_attributes, "name");
    require(palette_name && palette_name->value == "palette",
            "palette attribute mismatch");
    const ClassicDescription *description = find_classic_description(
        collection, "fur");
    require(description && description->objects.size() == 2u,
            "description objects mismatch");
    require(description->bindings.size() == 2u,
            "description bindings mismatch");
    require(description->patches.size() == 1u, "patch count mismatch");
    const ClassicPatch &patch = description->patches.front();
    require(patch.type == "Subd" && patch.name == "patchShape",
            "patch identity mismatch");
    require(patch.face_ids.size() == 1u && patch.face_ids[0] == 7u,
            "face IDs mismatch");
    require(find_classic_attribute(patch.attributes, "unknown") != nullptr,
            "unknown patch attribute was not preserved");
    require(patch.guides.size() == 1u, "guide count mismatch");
    const ClassicGuide &guide = patch.guides.front();
    require(guide.id == 42u && guide.face_id == 7u &&
                guide.patch_u == .25 && guide.patch_v == .75 && guide.blend == .5,
            "guide metadata mismatch");
    require(guide.interpolation_offset == 0u &&
                guide.interpolation_count == 3u &&
                patch.guide_interpolation == std::vector<double>({1.0, 2.0, 3.0}),
            "packed interpolation mismatch");
    require(guide.cv_offset == 0u && guide.cv_count == 3u &&
                patch.guide_cvs.size() == 3u,
            "packed guide CV range mismatch");
    require(patch.guide_cvs[0].x == 0.0 && patch.guide_cvs[0].y == 0.0 &&
                patch.guide_cvs[0].z == 0.0,
            "implicit guide root was not inserted");
    require(patch.guide_cvs[2].x == 4.0 && patch.guide_cvs[2].y == 5.0 &&
                patch.guide_cvs[2].z == 6.0,
            "guide CV payload mismatch");
}

void test_validation() {
    require_fails(fixture("42", ".25 .75 8"),
                  "guide face ID is not declared");
    require_fails(fixture("not-an-id"), "invalid guide ID");
    require_fails(fixture("42", ".25 nan 7"), "non-finite guide v");
    require_fails(fixture("42", ".25 .75 7", "4 inf 6"),
                  "non-finite guide CV");

    std::string duplicate = fixture();
    const std::string guide =
        "\tid\t42\n\tloc\t.25 .75 7\n\tblend\t.5\n"
        "\tinterp\t1:2:3\n\tCVs\t2\n\t1 2 3\n";
    const std::size_t patches = duplicate.find("Patches fur 1");
    duplicate.replace(patches, std::string("Patches fur 1").size(),
                      "Patches fur 2");
    const std::size_t guides = duplicate.find("Guides Spline 1");
    duplicate.replace(guides, std::string("Guides Spline 1").size(),
                      "Guides Spline 2");
    const std::size_t end = duplicate.find("endObject");
    duplicate.insert(end, guide);
    require_fails(duplicate, "duplicate guide ID");

    std::string count_mismatch = fixture();
    count_mismatch.replace(count_mismatch.find("Patches fur 1"),
                           std::string("Patches fur 1").size(),
                           "Patches fur 2");
    require_fails(count_mismatch, "Patches guide count does not match");
}

void test_limits() {
    ClassicParseLimits limits{};
    limits.max_line_bytes = 8u;
    require_fails(fixture(), "line byte limit exceeded", limits);
    limits = {};
    limits.max_guide_cvs = 2u;
    require_fails(fixture(), "guide CV limit exceeded", limits);
    limits = {};
    limits.max_source_bytes = 32u;
    require_fails(fixture(), "source byte limit exceeded", limits);
}

} // namespace

int main() try {
    test_typed_parse();
    test_validation();
    test_limits();
    std::cout << "Classic XGen parser tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
