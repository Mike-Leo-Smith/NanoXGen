#include "maya_xgen_cache_validation.h"
#include "nanoxgen/curve_cache.h"
#include "nanoxgen/xgen.h"

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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using nanoxgen::CurveCache;
using nanoxgen::CurveCacheHeader;
using nanoxgen::XGenCurveOrder;
using nanoxgen::XGenPackedCurves;

double elapsed_ms(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

class VectorOutputBuffer final : public std::streambuf {
public:
    [[nodiscard]] const std::vector<char> &bytes() const noexcept { return _bytes; }

protected:
    std::streamsize xsputn(const char *source, std::streamsize count) override {
        if (count <= 0) { return 0; }
        _bytes.insert(_bytes.end(), source, source + count);
        return count;
    }

    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        _bytes.push_back(traits_type::to_char_type(character));
        return character;
    }

private:
    std::vector<char> _bytes;
};

class XGenCacheCommand final : public MPxCommand {
public:
    static void *creator() { return new XGenCacheCommand; }

    MStatus doIt(const MArgList &args) override {
        try {
            if (args.length() < 2u || args.length() > 3u) {
                throw std::invalid_argument(
                    "usage: nanoxgenXGenCache node output.nxc [source-order|canonical]");
            }
            const MString node_name = args.asString(0u);
            const std::filesystem::path output_path{args.asString(1u).asChar()};
            const std::string mode =
                args.length() == 3u ? args.asString(2u).asChar() : "source-order";
            if (mode != "source-order" && mode != "canonical") {
                throw std::invalid_argument("mode must be source-order or canonical");
            }
            if (output_path.empty()) {
                throw std::invalid_argument("output path must not be empty");
            }

            MSelectionList selection;
            MStatus status = selection.add(node_name);
            if (!status) { throw std::runtime_error("failed to find the requested node"); }
            MObject node;
            status = selection.getDependNode(0u, node);
            if (!status) { throw std::runtime_error("failed to resolve the requested node"); }
            MFnDependencyNode function(node, &status);
            if (!status) {
                throw std::runtime_error("failed to construct dependency-node function set");
            }
            MPlug plug = function.findPlug("outRenderData", false, &status);
            if (!status) { throw std::runtime_error("node has no outRenderData plug"); }

            const auto total_begin = Clock::now();
            auto begin = Clock::now();
            MObject object = plug.asMObject(&status);
            const double evaluate_ms = elapsed_ms(begin, Clock::now());
            if (!status || object.isNull()) {
                throw std::runtime_error("outRenderData evaluation failed");
            }
            MFnPluginData plugin_data(object, &status);
            if (!status) { throw std::runtime_error("outRenderData is not plugin data"); }
            MPxData *data = plugin_data.data(&status);
            if (!status || !data) {
                throw std::runtime_error("outRenderData has no MPxData payload");
            }

            VectorOutputBuffer output_buffer;
            std::ostream blob_stream(&output_buffer);
            begin = Clock::now();
            status = data->writeBinary(blob_stream);
            blob_stream.flush();
            const double write_binary_ms = elapsed_ms(begin, Clock::now());
            if (!status || !blob_stream || output_buffer.bytes().empty()) {
                throw std::runtime_error("MPxData::writeBinary failed or returned no data");
            }
            const std::span<const std::byte> blob = std::as_bytes(
                std::span<const char>{output_buffer.bytes()});

            begin = Clock::now();
            const XGenPackedCurves packed = nanoxgen::parse_xgen_packed_curves(
                blob, mode == "source-order"
                    ? XGenCurveOrder::Source : XGenCurveOrder::Canonical);
            const CurveCache cache = nanoxgen::build_curve_cache(
                {packed.point_counts, packed.points, {}, {}, {}, {}, {}, {}});
            const double decode_cache_ms = elapsed_ms(begin, Clock::now());

            begin = Clock::now();
            nanoxgen::save_curve_cache(cache, output_path);
            const double nxc_write_ms = elapsed_ms(begin, Clock::now());
            const CurveCacheHeader header = cache.view().header();
            const double total_ms = elapsed_ms(total_begin, Clock::now());

            std::ostringstream json;
            json << std::setprecision(9)
                 << "{\"mode\":\"" << mode
                 << "\",\"blob_bytes\":" << output_buffer.bytes().size()
                 << ",\"curves\":" << header.strand_count
                 << ",\"points\":" << header.point_count
                 << ",\"evaluate_ms\":" << evaluate_ms
                 << ",\"write_binary_ms\":" << write_binary_ms
                 << ",\"decode_cache_ms\":" << decode_cache_ms
                 << ",\"nxc_write_ms\":" << nxc_write_ms
                 << ",\"total_ms\":" << total_ms
                 << ",\"content_hash\":\"0x" << std::hex << header.content_hash
                 << std::dec << "\",\"autodesk_serialization\":true"
                 << ",\"temporary_xgen_blob\":false"
                 << ",\"xgfnspline_reload\":false}";
            setResult(MString{json.str().c_str()});
            return MS::kSuccess;
        } catch (const std::exception &error) {
            MGlobal::displayError(
                MString{"NanoXGen in-memory XGen cache failed: "} + error.what());
            return MS::kFailure;
        }
    }
};

} // namespace

MStatus initializePlugin(MObject object) {
    MFnPlugin plugin(object, "NanoXGen", "0.2.0", "Any");
    return plugin.registerCommand("nanoxgenXGenCache", XGenCacheCommand::creator);
}

MStatus uninitializePlugin(MObject object) {
    MFnPlugin plugin(object);
    return plugin.deregisterCommand("nanoxgenXGenCache");
}
