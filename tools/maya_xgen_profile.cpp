#include <maya/MArgList.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFnPlugin.h>
#include <maya/MFnPluginData.h>
#include <maya/MGlobal.h>
#include <maya/MObject.h>
#include <maya/MPlug.h>
#include <maya/MPxCommand.h>
#include <maya/MPxData.h>
#include <maya/MSelectionList.h>
#include <maya/MStatus.h>
#include <maya/MString.h>

#if __has_include(<xgen/src/xgsculptcore/api/XgSplineAPI.h>)
#include <xgen/src/xgsculptcore/api/XgSplineAPI.h>
#elif __has_include(<XGen/XgSplineAPI.h>)
#include <XGen/XgSplineAPI.h>
#else
#error "Autodesk XgSplineAPI.h was not found"
#endif

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

class VectorOutputBuffer final : public std::streambuf {
public:
    explicit VectorOutputBuffer(std::size_t reserve_bytes = 0u) {
        _bytes.reserve(reserve_bytes);
    }

    [[nodiscard]] const std::vector<char> &bytes() const noexcept { return _bytes; }

protected:
    std::streamsize xsputn(const char *source, std::streamsize count) override {
        if (count > 0) {
            _bytes.insert(_bytes.end(), source, source + count);
        }
        return count;
    }

    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }
        _bytes.push_back(traits_type::to_char_type(ch));
        return ch;
    }

private:
    std::vector<char> _bytes;
};

class MemoryInputBuffer final : public std::streambuf {
public:
    explicit MemoryInputBuffer(const std::vector<char> &bytes) {
        char *begin = const_cast<char *>(bytes.data());
        setg(begin, begin, begin + bytes.size());
    }
};

struct PackedPoint {
    float x{};
    float y{};
    float z{};
    float radius{};
};

struct ProfileResult {
    std::string mode;
    std::uint64_t blob_bytes{};
    std::uint64_t batches{};
    std::uint64_t curves{};
    std::uint64_t vertices{};
    double evaluate_ms{};
    double write_binary_ms{};
    double stream_copy_ms{};
    double load_ms{};
    double execute_ms{};
    double materialize_ms{};
    double total_ms{};
    double checksum{};
};

void materialize(XGenSplineAPI::XgFnSpline &splines, ProfileResult &result) {
    for (auto it = splines.iterator(); !it.isDone(); it.next()) {
        ++result.batches;
        result.curves += it.primitiveCount();
        result.vertices += it.vertexCount();
    }
    std::vector<std::uint32_t> point_counts;
    std::vector<PackedPoint> points;
    point_counts.reserve(result.curves);
    points.reserve(result.vertices);
    const auto begin = Clock::now();
    for (auto it = splines.iterator(); !it.isDone(); it.next()) {
        const unsigned int stride = it.primitiveInfoStride();
        const unsigned int *infos = it.primitiveInfos();
        const SgVec3f *positions = it.positions();
        const float *widths = it.width();
        const unsigned int primitive_count = it.primitiveCount();
        const unsigned int vertex_count = it.vertexCount();
        if (stride < 2u || !infos || !positions || !widths) {
            throw std::runtime_error("XGen returned incomplete iterator data");
        }
        for (unsigned int primitive = 0u; primitive < primitive_count; ++primitive) {
            const unsigned int offset = infos[primitive * stride];
            const unsigned int length = infos[primitive * stride + 1u];
            if (offset > vertex_count || length > vertex_count - offset) {
                throw std::runtime_error("XGen returned an invalid primitive range");
            }
            point_counts.push_back(length);
            for (unsigned int vertex = offset; vertex < offset + length; ++vertex) {
                points.push_back({positions[vertex][0], positions[vertex][1],
                                  positions[vertex][2], 0.5f * widths[vertex]});
            }
        }
    }
    result.materialize_ms = elapsed_ms(begin, Clock::now());
    if (point_counts.size() != result.curves || points.size() != result.vertices) {
        throw std::runtime_error("materialized topology does not match iterator metadata");
    }
    if (!points.empty()) { result.checksum = points[points.size() / 2u].radius; }
}

class ProfileXGenDataCommand final : public MPxCommand {
public:
    static void *creator() { return new ProfileXGenDataCommand; }

    MStatus doIt(const MArgList &args) override {
        try {
            if (args.length() < 1u || args.length() > 3u) {
                throw std::invalid_argument(
                    "usage: nanoxgenProfileXGenData node [mode] [reserveBytes]");
            }
            ProfileResult result{};
            result.mode = args.length() >= 2u ? args.asString(1u).asChar() : "stringstream-copy";
            const std::size_t reserve_bytes = args.length() >= 3u
                ? static_cast<std::size_t>(args.asInt(2u)) : 0u;
            if (result.mode != "stringstream-copy" &&
                result.mode != "stringstream-no-copy" && result.mode != "vector") {
                throw std::invalid_argument("mode must be stringstream-copy, stringstream-no-copy, or vector");
            }

            MSelectionList selection;
            MStatus status = selection.add(args.asString(0u));
            if (!status) { throw std::runtime_error("failed to find the requested node"); }
            MObject node;
            status = selection.getDependNode(0u, node);
            if (!status) { throw std::runtime_error("failed to resolve the requested node"); }
            MFnDependencyNode function(node, &status);
            if (!status) { throw std::runtime_error("failed to construct dependency-node function set"); }
            MPlug plug = function.findPlug("outRenderData", false, &status);
            if (!status) { throw std::runtime_error("node has no outRenderData plug"); }

            const auto total_begin = Clock::now();
            auto begin = Clock::now();
            MObject object = plug.asMObject(&status);
            result.evaluate_ms = elapsed_ms(begin, Clock::now());
            if (!status || object.isNull()) { throw std::runtime_error("outRenderData evaluation failed"); }
            MFnPluginData plugin_data(object, &status);
            if (!status) { throw std::runtime_error("outRenderData is not plugin data"); }
            MPxData *data = plugin_data.data(&status);
            if (!status || !data) { throw std::runtime_error("outRenderData has no MPxData payload"); }

            XGenSplineAPI::XgFnSpline splines;
            if (result.mode == "vector") {
                VectorOutputBuffer output_buffer(reserve_bytes);
                std::ostream output(&output_buffer);
                begin = Clock::now();
                status = data->writeBinary(output);
                result.write_binary_ms = elapsed_ms(begin, Clock::now());
                if (!status || !output) { throw std::runtime_error("MPxData::writeBinary failed"); }
                result.blob_bytes = output_buffer.bytes().size();
                MemoryInputBuffer input_buffer(output_buffer.bytes());
                std::istream input(&input_buffer);
                begin = Clock::now();
                const bool loaded = splines.load(input, result.blob_bytes, 0.0f);
                result.load_ms = elapsed_ms(begin, Clock::now());
                if (!loaded) { throw std::runtime_error("XgFnSpline::load failed"); }
            } else {
                std::stringstream stream;
                begin = Clock::now();
                status = data->writeBinary(stream);
                result.write_binary_ms = elapsed_ms(begin, Clock::now());
                if (!status || !stream) { throw std::runtime_error("MPxData::writeBinary failed"); }
                const std::streampos end = stream.tellp();
                if (end < 0) { throw std::runtime_error("failed to query serialized size"); }
                result.blob_bytes = static_cast<std::uint64_t>(end);
                if (result.mode == "stringstream-copy") {
                    begin = Clock::now();
                    const std::string redundant_copy = stream.str();
                    result.stream_copy_ms = elapsed_ms(begin, Clock::now());
                    result.checksum += redundant_copy.empty() ? 0.0 :
                        static_cast<unsigned char>(redundant_copy.front()) * 0.0;
                }
                stream.clear();
                stream.seekg(0);
                begin = Clock::now();
                const bool loaded = splines.load(stream, result.blob_bytes, 0.0f);
                result.load_ms = elapsed_ms(begin, Clock::now());
                if (!loaded) { throw std::runtime_error("XgFnSpline::load failed"); }
            }

            begin = Clock::now();
            const bool executed = splines.executeScript();
            result.execute_ms = elapsed_ms(begin, Clock::now());
            if (!executed) { throw std::runtime_error("XgFnSpline::executeScript failed"); }
            materialize(splines, result);
            result.total_ms = elapsed_ms(total_begin, Clock::now());

            std::ostringstream json;
            json << std::setprecision(9)
                 << "{\"mode\":\"" << result.mode
                 << "\",\"blob_bytes\":" << result.blob_bytes
                 << ",\"batches\":" << result.batches
                 << ",\"curves\":" << result.curves
                 << ",\"vertices\":" << result.vertices
                 << ",\"evaluate_ms\":" << result.evaluate_ms
                 << ",\"write_binary_ms\":" << result.write_binary_ms
                 << ",\"stream_copy_ms\":" << result.stream_copy_ms
                 << ",\"load_ms\":" << result.load_ms
                 << ",\"execute_ms\":" << result.execute_ms
                 << ",\"materialize_ms\":" << result.materialize_ms
                 << ",\"total_ms\":" << result.total_ms
                 << ",\"checksum\":" << result.checksum << "}";
            setResult(MString{json.str().c_str()});
            return MS::kSuccess;
        } catch (const std::exception &error) {
            MGlobal::displayError(MString{"NanoXGen XGen profile failed: "} + error.what());
            return MS::kFailure;
        }
    }
};

} // namespace

MStatus initializePlugin(MObject object) {
    MFnPlugin plugin(object, "NanoXGen", "0.1.0", "Any");
    return plugin.registerCommand("nanoxgenProfileXGenData", ProfileXGenDataCommand::creator);
}

MStatus uninitializePlugin(MObject object) {
    MFnPlugin plugin(object);
    return plugin.deregisterCommand("nanoxgenProfileXGenData");
}
