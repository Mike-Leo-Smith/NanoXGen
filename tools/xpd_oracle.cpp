#include <xpd/src/core/Xpd.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct ReaderCloser {
    void operator()(XpdReader *reader) const noexcept {
        if (reader != nullptr) { reader->close(); }
    }
};

std::uint32_t parse_u32(const char *text) {
    std::size_t consumed{};
    const unsigned long value = std::stoul(text, &consumed);
    if (text[consumed] != '\0' ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("record limit is invalid");
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

int main(int argc, char **argv) try {
    if (argc != 2 && argc != 3) {
        throw std::invalid_argument(
            "usage: nanoxgen_xpd_oracle FILE.xuv [RECORD_LIMIT]");
    }
    const std::uint32_t limit = argc == 3 ? parse_u32(argv[2]) : 8u;
    std::unique_ptr<XpdReader, ReaderCloser> reader{XpdReader::open(argv[1])};
    if (!reader) { throw std::runtime_error("XpdReader::open failed"); }
    std::cout << std::setprecision(9)
              << "header prim_type " << static_cast<int>(reader->primType())
              << " prim_version " << static_cast<unsigned int>(reader->primVersion())
              << " coord_space " << static_cast<int>(reader->coordSpace())
              << " time " << reader->time()
              << " cvs " << reader->numCVs()
              << " faces " << reader->numFaces()
              << " blocks " << reader->blocks().size();
    for (const std::string &block : reader->blocks()) {
        std::cout << ' ' << block;
    }
    std::cout << '\n';
    std::uint32_t emitted{};
    while (reader->nextFace()) {
        const int face = reader->faceid();
        const unsigned int primitive_count = reader->numPrims();
        for (std::uint32_t block = 0u; block < reader->blocks().size(); ++block) {
            if (!reader->findBlock(block)) {
                throw std::runtime_error("XPD block seek failed");
            }
            for (unsigned int primitive = 0u; primitive < primitive_count;
                 ++primitive) {
                safevector<float> data;
                if (!reader->readPrim(data)) {
                    throw std::runtime_error("XPD primitive read failed");
                }
                if (emitted++ >= limit) { continue; }
                std::cout << "record face " << face << " block " << block
                          << " primitive " << primitive << " floats "
                          << data.size();
                for (const float value : data) { std::cout << ' ' << value; }
                std::cout << '\n';
            }
        }
    }
    std::cout << "records " << emitted << '\n';
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
