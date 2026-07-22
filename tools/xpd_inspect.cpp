#include "nanoxgen/xpd.h"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char **argv) try {
    if (argc != 2 && argc != 3) {
        throw std::invalid_argument(
            "usage: nanoxgen_xpd_inspect FILE.xpd [RECORD_LIMIT]");
    }
    std::uint32_t limit = 8u;
    if (argc == 3) {
        std::size_t consumed{};
        const unsigned long value = std::stoul(argv[2], &consumed);
        if (argv[2][consumed] != '\0' ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("record limit is invalid");
        }
        limit = static_cast<std::uint32_t>(value);
    }
    const nanoxgen::XpdDocument document =
        nanoxgen::load_xpd_document(argv[1]);
    std::cout << std::setprecision(9)
              << "header prim_type "
              << static_cast<std::uint32_t>(document.primitive_type)
              << " prim_version "
              << static_cast<std::uint32_t>(document.primitive_version)
              << " coord_space "
              << static_cast<std::uint32_t>(document.coordinate_space)
              << " time " << document.time
              << " cvs " << document.cv_count
              << " faces " << document.faces.size()
              << " blocks " << document.blocks.size();
    for (const auto &block : document.blocks) {
        std::cout << ' ' << block.name;
    }
    std::cout << '\n';
    std::uint64_t records{};
    for (std::size_t face_index = 0u;
         face_index < document.faces.size(); ++face_index) {
        const auto &face = document.faces[face_index];
        for (std::size_t block_index = 0u;
             block_index < document.blocks.size(); ++block_index) {
            const auto &block = document.blocks[block_index];
            std::vector<float> values(block.floats_per_primitive);
            for (std::uint32_t primitive = 0u;
                 primitive < face.primitive_count; ++primitive) {
                nanoxgen::copy_xpd_primitive(
                    document, face_index, block_index, primitive, values);
                if (records++ >= limit) { continue; }
                std::cout << "record face " << face.face_id << " block "
                          << block_index << " primitive " << primitive
                          << " floats " << values.size();
                for (const float value : values) { std::cout << ' ' << value; }
                std::cout << '\n';
            }
        }
    }
    std::cout << "records " << records << '\n';
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
