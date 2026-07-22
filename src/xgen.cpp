#include "nanoxgen/xgen.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace nanoxgen {
namespace {

constexpr std::size_t kFileHeaderBytes = 16u;
constexpr std::size_t kGroupHeaderBytes = 48u;
constexpr std::size_t kArrayHeaderBytes = 16u;
static_assert(std::endian::native == std::endian::little,
              "typed XGen array materialization requires a little-endian host");

[[noreturn]] void invalid(const std::string &message) {
    throw std::runtime_error("invalid XGen BLOB: " + message);
}

std::uint64_t checked_add(std::uint64_t a, std::uint64_t b, const char *label) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        invalid(std::string{label} + " size overflow");
    }
    return a + b;
}

std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t)) {
        invalid("truncated uint64 field");
    }
    std::uint64_t value = 0u;
    for (std::size_t byte = 0u; byte < sizeof(value); ++byte) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + byte]))
                 << (byte * 8u);
    }
    return value;
}

void append_u64(std::vector<std::byte> &bytes, std::uint64_t value) {
    for (std::size_t byte = 0u; byte < sizeof(value); ++byte) {
        bytes.push_back(static_cast<std::byte>((value >> (byte * 8u)) & 0xffu));
    }
}

void append_bytes(std::vector<std::byte> &destination, std::span<const std::byte> source) {
    if (source.size() > std::numeric_limits<std::size_t>::max() - destination.size()) {
        throw std::overflow_error("XGen serialization size overflows size_t");
    }
    destination.insert(destination.end(), source.begin(), source.end());
}

struct JsonValue {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type{Type::Null};
    bool boolean{};
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : _input(input) {}

    JsonValue parse() {
        JsonValue value = parse_value(0u);
        skip_space();
        if (_offset != _input.size()) { fail("trailing metadata characters"); }
        return value;
    }

private:
    [[noreturn]] void fail(const char *message) const {
        invalid(std::string{"metadata JSON "} + message + " at byte " +
                std::to_string(_offset));
    }

    void skip_space() {
        while (_offset < _input.size() &&
               (_input[_offset] == ' ' || _input[_offset] == '\n' ||
                _input[_offset] == '\r' || _input[_offset] == '\t')) {
            ++_offset;
        }
    }

    bool consume(char character) {
        skip_space();
        if (_offset < _input.size() && _input[_offset] == character) {
            ++_offset;
            return true;
        }
        return false;
    }

    void require(char character, const char *message) {
        if (!consume(character)) { fail(message); }
    }

    JsonValue parse_value(std::uint32_t depth) {
        if (depth > 64u) { fail("nesting exceeds 64 levels"); }
        skip_space();
        if (_offset >= _input.size()) { fail("is truncated"); }
        const char first = _input[_offset];
        if (first == '{') { return parse_object(depth + 1u); }
        if (first == '[') { return parse_array(depth + 1u); }
        if (first == '"') {
            JsonValue value{};
            value.type = JsonValue::Type::String;
            value.text = parse_string();
            return value;
        }
        if (first == 't') { return parse_literal("true", JsonValue::Type::Boolean, true); }
        if (first == 'f') { return parse_literal("false", JsonValue::Type::Boolean, false); }
        if (first == 'n') { return parse_literal("null", JsonValue::Type::Null, false); }
        if (first == '-' || (first >= '0' && first <= '9')) { return parse_number(); }
        fail("contains an unexpected token");
    }

    JsonValue parse_literal(
        std::string_view spelling, JsonValue::Type type, bool boolean) {
        if (_input.substr(_offset, spelling.size()) != spelling) {
            fail("contains an invalid literal");
        }
        _offset += spelling.size();
        JsonValue value{};
        value.type = type;
        value.boolean = boolean;
        return value;
    }

    static void append_utf8(std::string &output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fu) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else if (codepoint <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }

    std::uint32_t parse_hex4() {
        if (_input.size() - _offset < 4u) { fail("has a truncated unicode escape"); }
        std::uint32_t value = 0u;
        for (std::uint32_t digit = 0u; digit < 4u; ++digit) {
            const char c = _input[_offset++];
            value <<= 4u;
            if (c >= '0' && c <= '9') { value |= static_cast<std::uint32_t>(c - '0'); }
            else if (c >= 'a' && c <= 'f') { value |= static_cast<std::uint32_t>(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { value |= static_cast<std::uint32_t>(c - 'A' + 10); }
            else { fail("has an invalid unicode escape"); }
        }
        return value;
    }

    std::string parse_string() {
        require('"', "expected a string");
        std::string result;
        while (_offset < _input.size()) {
            const unsigned char c = static_cast<unsigned char>(_input[_offset++]);
            if (c == '"') { return result; }
            if (c < 0x20u) { fail("contains a control character in a string"); }
            if (c != '\\') {
                result.push_back(static_cast<char>(c));
                continue;
            }
            if (_offset >= _input.size()) { fail("has a truncated escape"); }
            switch (_input[_offset++]) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = parse_hex4();
                    if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                        if (_input.size() - _offset < 6u || _input[_offset] != '\\' ||
                            _input[_offset + 1u] != 'u') {
                            fail("has an incomplete unicode surrogate pair");
                        }
                        _offset += 2u;
                        const std::uint32_t low = parse_hex4();
                        if (low < 0xdc00u || low > 0xdfffu) {
                            fail("has an invalid unicode surrogate pair");
                        }
                        codepoint = 0x10000u + ((codepoint - 0xd800u) << 10u) +
                            (low - 0xdc00u);
                    } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
                        fail("has an unpaired low unicode surrogate");
                    }
                    append_utf8(result, codepoint);
                    break;
                }
                default: fail("contains an invalid escape");
            }
        }
        fail("has an unterminated string");
    }

    JsonValue parse_number() {
        const std::size_t begin = _offset;
        if (_input[_offset] == '-') { ++_offset; }
        if (_offset >= _input.size()) { fail("has a truncated number"); }
        if (_input[_offset] == '0') {
            ++_offset;
        } else {
            if (_input[_offset] < '1' || _input[_offset] > '9') {
                fail("has an invalid number");
            }
            while (_offset < _input.size() && _input[_offset] >= '0' &&
                   _input[_offset] <= '9') {
                ++_offset;
            }
        }
        if (_offset < _input.size() && _input[_offset] == '.') {
            ++_offset;
            const std::size_t fraction = _offset;
            while (_offset < _input.size() && _input[_offset] >= '0' &&
                   _input[_offset] <= '9') {
                ++_offset;
            }
            if (_offset == fraction) { fail("has an invalid fraction"); }
        }
        if (_offset < _input.size() &&
            (_input[_offset] == 'e' || _input[_offset] == 'E')) {
            ++_offset;
            if (_offset < _input.size() &&
                (_input[_offset] == '+' || _input[_offset] == '-')) {
                ++_offset;
            }
            const std::size_t exponent = _offset;
            while (_offset < _input.size() && _input[_offset] >= '0' &&
                   _input[_offset] <= '9') {
                ++_offset;
            }
            if (_offset == exponent) { fail("has an invalid exponent"); }
        }
        JsonValue value{};
        value.type = JsonValue::Type::Number;
        value.text.assign(_input.substr(begin, _offset - begin));
        return value;
    }

    JsonValue parse_array(std::uint32_t depth) {
        require('[', "expected an array");
        JsonValue result{};
        result.type = JsonValue::Type::Array;
        if (consume(']')) { return result; }
        for (;;) {
            result.array.push_back(parse_value(depth));
            if (consume(']')) { return result; }
            require(',', "expected ',' in an array");
        }
    }

    JsonValue parse_object(std::uint32_t depth) {
        require('{', "expected an object");
        JsonValue result{};
        result.type = JsonValue::Type::Object;
        if (consume('}')) { return result; }
        for (;;) {
            skip_space();
            if (_offset >= _input.size() || _input[_offset] != '"') {
                fail("expected an object key");
            }
            std::string key = parse_string();
            require(':', "expected ':' after an object key");
            if (std::any_of(result.object.begin(), result.object.end(),
                            [&](const auto &entry) { return entry.first == key; })) {
                fail("contains a duplicate object key");
            }
            result.object.emplace_back(std::move(key), parse_value(depth));
            if (consume('}')) { return result; }
            require(',', "expected ',' in an object");
        }
    }

    std::string_view _input;
    std::size_t _offset{};
};

const JsonValue &member(const JsonValue &object, std::string_view name) {
    if (object.type != JsonValue::Type::Object) {
        invalid("metadata member parent is not an object");
    }
    for (const auto &[key, value] : object.object) {
        if (key == name) { return value; }
    }
    invalid("metadata is missing '" + std::string{name} + "'");
}

std::uint64_t as_u64(const JsonValue &value, std::string_view name) {
    if (value.type != JsonValue::Type::Number || value.text.empty() ||
        value.text.front() == '-' || value.text.find_first_of(".eE") != std::string::npos) {
        invalid("metadata '" + std::string{name} + "' is not an unsigned integer");
    }
    std::uint64_t result = 0u;
    const char *begin = value.text.data();
    const char *end = begin + value.text.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        invalid("metadata '" + std::string{name} + "' is outside uint64");
    }
    return result;
}

bool as_bool(const JsonValue &value, std::string_view name) {
    if (value.type != JsonValue::Type::Boolean) {
        invalid("metadata '" + std::string{name} + "' is not boolean");
    }
    return value.boolean;
}

const std::string &as_string(const JsonValue &value, std::string_view name) {
    if (value.type != JsonValue::Type::String) {
        invalid("metadata '" + std::string{name} + "' is not a string");
    }
    return value.text;
}

const std::vector<JsonValue> &as_array(const JsonValue &value, std::string_view name) {
    if (value.type != JsonValue::Type::Array) {
        invalid("metadata '" + std::string{name} + "' is not an array");
    }
    return value.array;
}

struct MetadataHeader {
    JsonValue root;
    std::uint32_t version{};
    std::uint32_t group_version{};
    std::uint32_t group_count{};
    bool group_base64{};
    bool group_deflate{};
    std::uint32_t group_deflate_level{};
};

MetadataHeader parse_metadata(std::string_view json) {
    MetadataHeader result{};
    result.root = JsonParser{json}.parse();
    const JsonValue &header = member(result.root, "Header");
    const std::uint64_t version = as_u64(member(header, "Version"), "Version");
    const std::uint64_t group_version =
        as_u64(member(header, "GroupVersion"), "GroupVersion");
    const std::uint64_t group_count = as_u64(member(header, "GroupCount"), "GroupCount");
    const std::uint64_t deflate_level =
        as_u64(member(header, "GroupDeflateLevel"), "GroupDeflateLevel");
    if (version > std::numeric_limits<std::uint32_t>::max() ||
        group_version > std::numeric_limits<std::uint32_t>::max() ||
        group_count > std::numeric_limits<std::uint32_t>::max() ||
        deflate_level > 9u) {
        invalid("metadata header value is outside its supported range");
    }
    if (as_string(member(header, "Type"), "Type") != "XgSplineData") {
        invalid("metadata Type is not XgSplineData");
    }
    result.version = static_cast<std::uint32_t>(version);
    result.group_version = static_cast<std::uint32_t>(group_version);
    result.group_count = static_cast<std::uint32_t>(group_count);
    result.group_base64 = as_bool(member(header, "GroupBase64"), "GroupBase64");
    result.group_deflate = as_bool(member(header, "GroupDeflate"), "GroupDeflate");
    result.group_deflate_level = static_cast<std::uint32_t>(deflate_level);
    if (result.version != 1u || result.group_version != 1u) {
        invalid("unsupported XgSplineData or group version");
    }
    if (result.group_base64) {
        invalid("GroupBase64=true ASCII streams are not supported by the binary BLOB API");
    }
    return result;
}

std::vector<std::byte> inflate_group(
    std::span<const std::byte> stored, std::uint64_t expected_size) {
    if (expected_size > std::numeric_limits<std::size_t>::max() ||
        expected_size > std::numeric_limits<uLongf>::max() ||
        stored.size() > std::numeric_limits<uLong>::max()) {
        invalid("compressed group exceeds zlib addressable size");
    }
    std::vector<std::byte> output(
        std::max<std::size_t>(static_cast<std::size_t>(expected_size), 1u));
    uLongf output_size = static_cast<uLongf>(output.size());
    const int status = uncompress(
        reinterpret_cast<Bytef *>(output.data()), &output_size,
        reinterpret_cast<const Bytef *>(stored.data()), static_cast<uLong>(stored.size()));
    if (status != Z_OK || output_size != expected_size) {
        invalid("group deflate stream is corrupt or has the wrong size");
    }
    output.resize(static_cast<std::size_t>(expected_size));
    return output;
}

std::vector<std::byte> deflate_group(
    std::span<const std::byte> raw, std::uint32_t level) {
    if (raw.size() > std::numeric_limits<uLong>::max()) {
        throw std::overflow_error("XGen group exceeds zlib addressable size");
    }
    const uLong bound = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::byte> output(static_cast<std::size_t>(bound));
    uLongf output_size = bound;
    const int status = compress2(
        reinterpret_cast<Bytef *>(output.data()), &output_size,
        reinterpret_cast<const Bytef *>(raw.data()), static_cast<uLong>(raw.size()),
        static_cast<int>(level));
    if (status != Z_OK) { throw std::runtime_error("failed to deflate XGen group"); }
    output.resize(static_cast<std::size_t>(output_size));
    return output;
}

std::string json_escape(std::string_view text) {
    std::ostringstream output;
    for (const unsigned char byte : text) {
        switch (byte) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (byte < 0x20u) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned int>(byte) << std::dec;
                } else {
                    output << static_cast<char>(byte);
                }
        }
    }
    return output.str();
}

template<typename T>
XGenArray make_array(std::uint64_t tag, std::span<const T> values) {
    static_assert(std::is_trivially_copyable_v<T>);
    XGenArray result{};
    result.type_tag = tag;
    const std::span<const std::byte> bytes = std::as_bytes(values);
    result.bytes.assign(bytes.begin(), bytes.end());
    return result;
}

Vec3 normalize_or(Vec3 value, Vec3 fallback) {
    const float squared = length_squared(value);
    if (!std::isfinite(squared) || squared <= 1.0e-20f) { return fallback; }
    return value / std::sqrt(squared);
}

bool finite(Vec2 value) noexcept;
bool finite(Vec3 value) noexcept;

void validate_materialized(const XGenEvaluatedCurves &curves) {
    if (curves.point_counts.empty()) { invalid("curve set is empty"); }
    const std::size_t curve_count = curves.point_counts.size();
    if (curves.patch_uvs.size() != curve_count ||
        curves.face_uvs.size() != curve_count || curves.face_ids.size() != curve_count) {
        invalid("curve uniform channel sizes do not agree");
    }
    std::uint64_t point_count = 0u;
    for (const std::uint32_t count : curves.point_counts) {
        if (count < 2u) { invalid("curve has fewer than two CVs"); }
        point_count = checked_add(point_count, count, "curve point");
    }
    if (point_count != curves.positions.size() || point_count != curves.widths.size() ||
        (!curves.texcoords.empty() && point_count != curves.texcoords.size())) {
        invalid("curve varying channel sizes do not agree");
    }
    for (std::size_t curve = 0u; curve < curve_count; ++curve) {
        if (!finite(curves.patch_uvs[curve]) || !finite(curves.face_uvs[curve])) {
            invalid("curve set contains non-finite UVs");
        }
    }
    for (std::size_t point = 0u; point < curves.positions.size(); ++point) {
        if (!finite(curves.positions[point]) || !std::isfinite(curves.widths[point]) ||
            curves.widths[point] < 0.0f ||
            (!curves.texcoords.empty() && !finite(curves.texcoords[point]))) {
            invalid("curve set contains invalid varying data");
        }
    }
}

const XGenArray &array_by_id(const XGenDocument &document, std::uint64_t id) {
    const std::uint32_t group_index = static_cast<std::uint32_t>(id >> 32u);
    const std::uint32_t array_index = static_cast<std::uint32_t>(id);
    const auto group = std::find_if(
        document.groups.begin(), document.groups.end(),
        [&](const XGenGroup &candidate) { return candidate.index == group_index; });
    if (group == document.groups.end() || array_index >= group->arrays.size()) {
        invalid("metadata array reference is out of bounds");
    }
    return group->arrays[array_index];
}

XGenArray &array_by_id(XGenDocument &document, std::uint64_t id) {
    const std::uint32_t group_index = static_cast<std::uint32_t>(id >> 32u);
    const std::uint32_t array_index = static_cast<std::uint32_t>(id);
    if (group_index >= document.groups.size() ||
        document.groups[group_index].index != group_index ||
        array_index >= document.groups[group_index].arrays.size()) {
        invalid("metadata array reference is out of bounds");
    }
    return document.groups[group_index].arrays[array_index];
}

template<typename T>
std::vector<T> copy_typed_array(
    const XGenDocument &document, const JsonValue &item, std::string_view name,
    std::uint64_t expected_tag) {
    const XGenArray &array = array_by_id(document, as_u64(member(item, name), name));
    if (array.type_tag != expected_tag) {
        invalid("array '" + std::string{name} + "' has an unexpected type tag");
    }
    if (array.bytes.size() % sizeof(T) != 0u) {
        invalid("array '" + std::string{name} + "' has a partial element");
    }
    std::vector<T> result(array.bytes.size() / sizeof(T));
    if (!result.empty()) {
        std::memcpy(result.data(), array.bytes.data(), array.bytes.size());
    }
    return result;
}

bool finite(Vec2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool finite(Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

struct MaterializedCurve {
    std::uint32_t face_id{};
    Vec2 face_uv{};
    Vec2 patch_uv{};
    std::vector<Vec3> positions;
    std::vector<float> widths;
    std::vector<Vec2> texcoords;
};

std::uint32_t float_bits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

auto curve_key(const MaterializedCurve &curve) noexcept {
    return std::tuple{curve.face_id, float_bits(curve.face_uv.x),
                      float_bits(curve.face_uv.y), float_bits(curve.patch_uv.x),
                      float_bits(curve.patch_uv.y)};
}

struct PackedItemRefs {
    std::uint64_t primitive_infos{};
    std::uint64_t positions{};
    std::uint64_t widths{};
    std::uint64_t patch_uvs{};
    std::uint64_t face_uvs{};
    std::uint64_t face_ids{};
};

std::vector<PackedItemRefs> packed_item_refs(
    const MetadataHeader &metadata, XGenCurveOrder order) {
    const std::vector<JsonValue> &items = as_array(
        member(metadata.root, "Items"), "Items");
    if (items.empty()) { invalid("metadata contains no Items"); }
    std::vector<PackedItemRefs> result;
    result.reserve(items.size());
    for (const JsonValue &item : items) {
        PackedItemRefs refs{};
        refs.primitive_infos = as_u64(
            member(item, "PrimitiveInfos"), "PrimitiveInfos");
        refs.positions = as_u64(member(item, "Positions"), "Positions");
        refs.widths = as_u64(member(item, "WIDTH_CV"), "WIDTH_CV");
        if (order == XGenCurveOrder::Canonical) {
            refs.patch_uvs = as_u64(member(item, "PatchUVs"), "PatchUVs");
            refs.face_uvs = as_u64(member(item, "FaceUV"), "FaceUV");
            refs.face_ids = as_u64(member(item, "FaceId"), "FaceId");
        }
        result.push_back(refs);
    }
    return result;
}

struct PackedArrayView {
    std::uint64_t type_tag{};
    std::span<const std::byte> bytes;
};

template<typename T>
std::size_t packed_array_size(
    const PackedArrayView &array, std::uint64_t expected_tag,
    std::string_view name) {
    if (array.type_tag != expected_tag) {
        invalid("array '" + std::string{name} + "' has an unexpected type tag");
    }
    if (array.bytes.size() % sizeof(T) != 0u) {
        invalid("array '" + std::string{name} + "' has a partial element");
    }
    return array.bytes.size() / sizeof(T);
}

template<typename T>
T packed_array_value(
    const PackedArrayView &array, std::size_t index,
    std::string_view name) {
    if (index >= array.bytes.size() / sizeof(T)) {
        invalid("array '" + std::string{name} + "' index is out of bounds");
    }
    T result{};
    std::memcpy(&result, array.bytes.data() + index * sizeof(T), sizeof(T));
    return result;
}

struct PackedItemViews {
    PackedArrayView primitive_infos;
    PackedArrayView positions;
    PackedArrayView widths;
    PackedArrayView patch_uvs;
    PackedArrayView face_uvs;
    PackedArrayView face_ids;
    std::size_t primitive_count{};
    std::size_t primitive_stride{};
    std::size_t position_count{};
};

using PackedCurveKey = std::tuple<
    std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;

struct PackedCurveRange {
    PackedCurveKey key;
    std::size_t item{};
    std::uint32_t first{};
    std::uint32_t count{};
};

template<typename Lookup>
XGenPackedCurves pack_referenced_arrays(
    const std::vector<PackedItemRefs> &refs, XGenCurveOrder order,
    Lookup &&lookup) {
    constexpr std::size_t source_primitive_stride = 3u;
    std::vector<PackedItemViews> items;
    items.reserve(refs.size());
    std::uint64_t curve_total = 0u;
    for (const PackedItemRefs &item_refs : refs) {
        PackedItemViews item{};
        item.primitive_infos = lookup(item_refs.primitive_infos);
        item.positions = lookup(item_refs.positions);
        item.widths = lookup(item_refs.widths);
        const std::size_t info_count = packed_array_size<std::uint32_t>(
            item.primitive_infos, kXGenUInt32ArrayTag, "PrimitiveInfos");
        item.position_count = packed_array_size<Vec3>(
            item.positions, kXGenVec3ArrayTag, "Positions");
        const std::size_t width_count = packed_array_size<float>(
            item.widths, kXGenFloatArrayTag, "WIDTH_CV");
        if (item.position_count != width_count) {
            invalid("Item Positions and WIDTH_CV sizes do not agree");
        }
        if (order == XGenCurveOrder::Source) {
            // Version-1 XgSplineData stores PrimitiveInfos as
            // (offset, count, flags). This is the public iterator's observed
            // stride in Maya 2027 and the layout emitted by NanoXGen's writer.
            if (info_count == 0u || info_count % source_primitive_stride != 0u) {
                invalid("source-order PrimitiveInfos does not use the v1 three-word stride");
            }
            item.primitive_stride = source_primitive_stride;
            item.primitive_count = info_count / source_primitive_stride;
        } else {
            item.patch_uvs = lookup(item_refs.patch_uvs);
            item.face_uvs = lookup(item_refs.face_uvs);
            item.face_ids = lookup(item_refs.face_ids);
            const std::size_t patch_uv_count = packed_array_size<Vec2>(
                item.patch_uvs, kXGenVec2ArrayTag, "PatchUVs");
            const std::size_t face_uv_count = packed_array_size<Vec2>(
                item.face_uvs, kXGenVec2ArrayTag, "FaceUV");
            item.primitive_count = packed_array_size<std::uint32_t>(
                item.face_ids, kXGenUInt32ArrayTag, "FaceId");
            if (item.primitive_count == 0u ||
                face_uv_count != item.primitive_count ||
                info_count % item.primitive_count != 0u ||
                patch_uv_count != item.position_count) {
                invalid("Item canonical metadata sizes do not agree");
            }
            item.primitive_stride = info_count / item.primitive_count;
            if (item.primitive_stride < 2u) {
                invalid("Item primitive info stride is too small");
            }
        }
        curve_total = checked_add(curve_total, item.primitive_count, "curve");
        items.push_back(item);
    }
    if (curve_total == 0u || curve_total > std::numeric_limits<std::size_t>::max()) {
        invalid("Items contain no curves or exceed size_t");
    }

    std::vector<PackedCurveRange> canonical_ranges;
    if (order == XGenCurveOrder::Canonical) {
        canonical_ranges.reserve(static_cast<std::size_t>(curve_total));
    }
    std::uint64_t point_total = 0u;
    for (std::size_t item_index = 0u; item_index < items.size(); ++item_index) {
        const PackedItemViews &item = items[item_index];
        for (std::size_t primitive = 0u;
             primitive < item.primitive_count; ++primitive) {
            const std::size_t info = primitive * item.primitive_stride;
            const std::uint32_t first = packed_array_value<std::uint32_t>(
                item.primitive_infos, info, "PrimitiveInfos");
            const std::uint32_t count = packed_array_value<std::uint32_t>(
                item.primitive_infos, info + 1u, "PrimitiveInfos");
            if (count < 2u || first > item.position_count ||
                count > item.position_count - first) {
                invalid("Item primitive range is out of bounds");
            }
            point_total = checked_add(point_total, count, "curve point");
            if (order == XGenCurveOrder::Canonical) {
                const Vec2 face_uv = packed_array_value<Vec2>(
                    item.face_uvs, primitive, "FaceUV");
                const Vec2 patch_uv = packed_array_value<Vec2>(
                    item.patch_uvs, first, "PatchUVs");
                if (!finite(face_uv) || !finite(patch_uv)) {
                    invalid("Item contains a non-finite curve UV");
                }
                canonical_ranges.push_back({
                    {packed_array_value<std::uint32_t>(
                         item.face_ids, primitive, "FaceId"),
                     float_bits(face_uv.x), float_bits(face_uv.y),
                     float_bits(patch_uv.x), float_bits(patch_uv.y)},
                    item_index, first, count});
            }
        }
    }
    if (point_total > std::numeric_limits<std::size_t>::max()) {
        invalid("packed point count exceeds size_t");
    }
    if (order == XGenCurveOrder::Canonical) {
        std::stable_sort(
            canonical_ranges.begin(), canonical_ranges.end(),
            [](const PackedCurveRange &a, const PackedCurveRange &b) {
                return a.key < b.key;
            });
        for (std::size_t curve = 1u; curve < canonical_ranges.size(); ++curve) {
            if (canonical_ranges[curve - 1u].key == canonical_ranges[curve].key) {
                invalid("duplicate canonical curve identity");
            }
        }
    }

    XGenPackedCurves output{};
    output.point_counts.resize(static_cast<std::size_t>(curve_total));
    output.points.resize(static_cast<std::size_t>(point_total));
    std::size_t output_curve = 0u;
    std::size_t output_point = 0u;
    const auto emit = [&](const PackedItemViews &item, std::uint32_t first,
                          std::uint32_t count, bool validate_patch_uvs) {
        output.point_counts[output_curve++] = count;
        for (std::uint32_t cv = 0u; cv < count; ++cv) {
            const std::size_t index = static_cast<std::size_t>(first) + cv;
            const Vec3 position = packed_array_value<Vec3>(
                item.positions, index, "Positions");
            const float width = packed_array_value<float>(
                item.widths, index, "WIDTH_CV");
            if (!finite(position) || !std::isfinite(width) || width < 0.0f) {
                invalid("Item contains an invalid renderer value");
            }
            if (validate_patch_uvs && !finite(packed_array_value<Vec2>(
                    item.patch_uvs, index, "PatchUVs"))) {
                invalid("Item contains a non-finite patch UV");
            }
            output.points[output_point++] = {
                position.x, position.y, position.z, 0.5f * width};
        }
    };
    if (order == XGenCurveOrder::Source) {
        for (const PackedItemViews &item : items) {
            for (std::size_t primitive = 0u;
                 primitive < item.primitive_count; ++primitive) {
                const std::size_t info = primitive * item.primitive_stride;
                emit(item,
                     packed_array_value<std::uint32_t>(
                         item.primitive_infos, info, "PrimitiveInfos"),
                     packed_array_value<std::uint32_t>(
                         item.primitive_infos, info + 1u, "PrimitiveInfos"),
                     false);
            }
        }
    } else {
        for (const PackedCurveRange &range : canonical_ranges) {
            emit(items[range.item], range.first, range.count, true);
        }
    }
    if (output_curve != output.point_counts.size() ||
        output_point != output.points.size()) {
        invalid("packed output counts do not match validated topology");
    }
    return output;
}

XGenPackedCurves pack_document_arrays(
    const XGenDocument &document, XGenCurveOrder order) {
    const MetadataHeader metadata = parse_metadata(document.metadata_json);
    const std::vector<PackedItemRefs> refs = packed_item_refs(metadata, order);
    return pack_referenced_arrays(refs, order, [&](std::uint64_t id) {
        const XGenArray &array = array_by_id(document, id);
        return PackedArrayView{array.type_tag, array.bytes};
    });
}

struct SelectedPackedArray {
    std::uint64_t id{};
    std::uint64_t expected_tag{};
    std::string name;
    std::vector<std::byte> bytes;
    bool loaded{};
};

XGenPackedCurves parse_fused_packed_curves(
    std::span<const std::byte> bytes, XGenCurveOrder order) {
    if (bytes.size() < kFileHeaderBytes) { invalid("file header is truncated"); }
    if (read_u64(bytes, 0u) != kXGenFileMagic) { invalid("file magic does not match"); }
    const std::uint64_t metadata_size = read_u64(bytes, 8u);
    if (metadata_size > bytes.size() - kFileHeaderBytes) {
        invalid("metadata length exceeds the file");
    }
    const std::size_t metadata_bytes = static_cast<std::size_t>(metadata_size);
    const std::string_view metadata_json{
        reinterpret_cast<const char *>(bytes.data() + kFileHeaderBytes),
        metadata_bytes};
    const MetadataHeader metadata = parse_metadata(metadata_json);
    const std::vector<PackedItemRefs> refs = packed_item_refs(metadata, order);

    std::vector<SelectedPackedArray> selected;
    std::map<std::uint64_t, std::size_t> selected_slots;
    const auto select = [&](std::uint64_t id, std::uint64_t tag,
                            const char *name) {
        const auto existing = selected_slots.find(id);
        if (existing != selected_slots.end()) {
            if (selected[existing->second].expected_tag != tag) {
                invalid("one array is referenced with conflicting renderer types");
            }
            return;
        }
        selected_slots.emplace(id, selected.size());
        selected.push_back({id, tag, name, {}, false});
    };
    for (const PackedItemRefs &item : refs) {
        select(item.primitive_infos, kXGenUInt32ArrayTag, "PrimitiveInfos");
        select(item.positions, kXGenVec3ArrayTag, "Positions");
        select(item.widths, kXGenFloatArrayTag, "WIDTH_CV");
        if (order == XGenCurveOrder::Canonical) {
            select(item.patch_uvs, kXGenVec2ArrayTag, "PatchUVs");
            select(item.face_uvs, kXGenVec2ArrayTag, "FaceUV");
            select(item.face_ids, kXGenUInt32ArrayTag, "FaceId");
        }
    }

    struct PackedStoredGroup {
        std::uint32_t index{};
        std::uint64_t array_count{};
        std::uint64_t raw_data_size{};
        std::size_t payload_offset{};
        std::size_t payload_size{};
    };
    std::vector<PackedStoredGroup> stored_groups;
    stored_groups.reserve(metadata.group_count);
    std::size_t offset = kFileHeaderBytes + metadata_bytes;
    if (metadata.group_count > (bytes.size() - offset) / kGroupHeaderBytes) {
        invalid("group count exceeds the remaining file");
    }
    for (std::uint32_t ordinal = 0u; ordinal < metadata.group_count; ++ordinal) {
        if (offset > bytes.size() || bytes.size() - offset < kGroupHeaderBytes) {
            invalid("group header is truncated");
        }
        if (read_u64(bytes, offset) != kXGenGroupMagic) {
            invalid("group magic does not match");
        }
        const std::uint64_t stored_size = read_u64(bytes, offset + 8u);
        const std::uint64_t group_index_u64 = read_u64(bytes, offset + 16u);
        const std::uint64_t array_count = read_u64(bytes, offset + 24u);
        const std::uint64_t raw_data_size = read_u64(bytes, offset + 32u);
        if (stored_size < 32u) { invalid("group stored size is out of bounds"); }
        if (group_index_u64 != ordinal || group_index_u64 >= metadata.group_count) {
            invalid("groups are not stored in index order");
        }
        if (array_count > (std::numeric_limits<std::uint64_t>::max() - raw_data_size) /
                              kArrayHeaderBytes) {
            invalid("group expanded size overflows uint64");
        }
        const std::uint64_t payload_size_u64 = stored_size - 32u;
        if (payload_size_u64 > bytes.size() - offset - kGroupHeaderBytes) {
            invalid("group stored size is out of bounds");
        }
        stored_groups.push_back({
            ordinal, array_count, raw_data_size, offset + kGroupHeaderBytes,
            static_cast<std::size_t>(payload_size_u64)});
        offset += kGroupHeaderBytes + static_cast<std::size_t>(payload_size_u64);
    }
    if (offset != bytes.size()) { invalid("file has trailing bytes after its groups"); }

    std::vector<std::vector<std::pair<std::uint32_t, std::size_t>>> group_selections(
        metadata.group_count);
    for (std::size_t slot = 0u; slot < selected.size(); ++slot) {
        const std::uint32_t group = static_cast<std::uint32_t>(selected[slot].id >> 32u);
        const std::uint32_t array = static_cast<std::uint32_t>(selected[slot].id);
        if (group >= stored_groups.size() ||
            array >= stored_groups[group].array_count) {
            invalid("metadata renderer array reference is out of bounds");
        }
        group_selections[group].emplace_back(array, slot);
    }
    for (auto &selection : group_selections) {
        std::sort(selection.begin(), selection.end());
    }

    const auto decode_group = [&](const PackedStoredGroup &stored_group) {
        const std::span<const std::byte> stored = bytes.subspan(
            stored_group.payload_offset, stored_group.payload_size);
        const std::uint64_t expanded_size = checked_add(
            stored_group.raw_data_size,
            stored_group.array_count * kArrayHeaderBytes,
            "group expanded");
        std::vector<std::byte> inflated;
        std::span<const std::byte> raw;
        if (metadata.group_deflate) {
            inflated = inflate_group(stored, expanded_size);
            raw = inflated;
        } else {
            if (stored.size() != expanded_size) {
                invalid("uncompressed group size does not match its header");
            }
            raw = stored;
        }
        const auto &selection = group_selections[stored_group.index];
        std::size_t selected_index = 0u;
        std::size_t array_offset = 0u;
        std::uint64_t data_sum = 0u;
        for (std::uint64_t array_index = 0u;
             array_index < stored_group.array_count; ++array_index) {
            if (array_offset > raw.size() || raw.size() - array_offset < kArrayHeaderBytes) {
                invalid("array record header is truncated");
            }
            const std::uint64_t tag = read_u64(raw, array_offset);
            const std::uint64_t array_size = read_u64(raw, array_offset + 8u);
            array_offset += kArrayHeaderBytes;
            if (array_size > raw.size() - array_offset) {
                invalid("array record data is truncated");
            }
            const std::size_t size = static_cast<std::size_t>(array_size);
            if (selected_index < selection.size() &&
                selection[selected_index].first == array_index) {
                SelectedPackedArray &target = selected[selection[selected_index].second];
                if (tag != target.expected_tag) {
                    invalid("array '" + target.name + "' has an unexpected type tag");
                }
                target.bytes.assign(
                    raw.begin() + static_cast<std::ptrdiff_t>(array_offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(array_offset + size));
                target.loaded = true;
                ++selected_index;
            }
            array_offset += size;
            data_sum = checked_add(data_sum, array_size, "group raw data");
        }
        if (array_offset != raw.size() || data_sum != stored_group.raw_data_size ||
            selected_index != selection.size()) {
            invalid("group array sizes do not consume the declared payload");
        }
    };

    const std::size_t worker_count = std::min<std::size_t>(
        stored_groups.size(), std::max<std::size_t>(
            1u, std::min<std::size_t>(8u, std::thread::hardware_concurrency())));
    if (worker_count <= 1u) {
        for (const PackedStoredGroup &group : stored_groups) { decode_group(group); }
    } else {
        std::atomic_size_t next_group{0u};
        std::atomic_bool failed{false};
        std::exception_ptr first_error;
        std::mutex error_mutex;
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0u; worker < worker_count; ++worker) {
            workers.emplace_back([&] {
                while (!failed.load(std::memory_order_relaxed)) {
                    const std::size_t ordinal = next_group.fetch_add(
                        1u, std::memory_order_relaxed);
                    if (ordinal >= stored_groups.size()) { return; }
                    try {
                        decode_group(stored_groups[ordinal]);
                    } catch (...) {
                        std::lock_guard lock{error_mutex};
                        if (!first_error) { first_error = std::current_exception(); }
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }
        workers.clear();
        if (first_error) { std::rethrow_exception(first_error); }
    }
    for (const SelectedPackedArray &array : selected) {
        if (!array.loaded) { invalid("referenced renderer array was not decoded"); }
    }
    return pack_referenced_arrays(refs, order, [&](std::uint64_t id) {
        const auto slot = selected_slots.find(id);
        if (slot == selected_slots.end()) {
            invalid("renderer array was not selected");
        }
        const SelectedPackedArray &array = selected[slot->second];
        return PackedArrayView{array.expected_tag, array.bytes};
    });
}

} // namespace

XGenDocument parse_xgen_document(std::span<const std::byte> bytes) {
    if (bytes.size() < kFileHeaderBytes) { invalid("file header is truncated"); }
    if (read_u64(bytes, 0u) != kXGenFileMagic) { invalid("file magic does not match"); }
    const std::uint64_t metadata_size = read_u64(bytes, 8u);
    if (metadata_size > bytes.size() - kFileHeaderBytes) {
        invalid("metadata length exceeds the file");
    }
    const std::size_t metadata_bytes = static_cast<std::size_t>(metadata_size);
    const auto *metadata_data = reinterpret_cast<const char *>(bytes.data() + kFileHeaderBytes);
    XGenDocument document{};
    document.metadata_json.assign(metadata_data, metadata_bytes);
    MetadataHeader metadata = parse_metadata(document.metadata_json);
    document.version = metadata.version;
    document.group_version = metadata.group_version;
    document.group_base64 = metadata.group_base64;
    document.group_deflate = metadata.group_deflate;
    document.group_deflate_level = metadata.group_deflate_level;
    document.groups.resize(metadata.group_count);

    struct StoredGroup {
        std::uint32_t index{};
        std::uint64_t array_count{};
        std::uint64_t raw_data_size{};
        std::uint64_t flags{};
        std::size_t payload_offset{};
        std::size_t payload_size{};
    };
    std::vector<StoredGroup> stored_groups;
    stored_groups.reserve(metadata.group_count);

    std::size_t offset = kFileHeaderBytes + metadata_bytes;
    if (metadata.group_count > (bytes.size() - offset) / kGroupHeaderBytes) {
        invalid("group count exceeds the remaining file");
    }
    std::vector<bool> seen_groups(metadata.group_count, false);
    for (std::uint32_t ordinal = 0u; ordinal < metadata.group_count; ++ordinal) {
        if (offset > bytes.size() || bytes.size() - offset < kGroupHeaderBytes) {
            invalid("group header is truncated");
        }
        if (read_u64(bytes, offset) != kXGenGroupMagic) {
            invalid("group magic does not match");
        }
        const std::uint64_t stored_size = read_u64(bytes, offset + 8u);
        if (stored_size < 32u) {
            invalid("group stored size is out of bounds");
        }
        const std::uint64_t group_index_u64 = read_u64(bytes, offset + 16u);
        const std::uint64_t array_count = read_u64(bytes, offset + 24u);
        const std::uint64_t raw_data_size = read_u64(bytes, offset + 32u);
        const std::uint64_t flags = read_u64(bytes, offset + 40u);
        if (group_index_u64 >= metadata.group_count) {
            invalid("group index is out of range");
        }
        const std::uint32_t group_index = static_cast<std::uint32_t>(group_index_u64);
        if (group_index != ordinal) { invalid("groups are not stored in index order"); }
        if (seen_groups[group_index]) { invalid("group index is duplicated"); }
        seen_groups[group_index] = true;
        if (array_count > (std::numeric_limits<std::uint64_t>::max() - raw_data_size) /
                              kArrayHeaderBytes) {
            invalid("group expanded size overflows uint64");
        }
        const std::uint64_t payload_size_u64 = stored_size - 32u;
        if (payload_size_u64 > bytes.size() - offset - kGroupHeaderBytes) {
            invalid("group stored size is out of bounds");
        }
        const std::size_t payload_offset = offset + kGroupHeaderBytes;
        const std::size_t payload_size = static_cast<std::size_t>(payload_size_u64);
        stored_groups.push_back(
            {group_index, array_count, raw_data_size, flags, payload_offset, payload_size});
        offset += kGroupHeaderBytes + payload_size;
    }
    if (offset != bytes.size()) { invalid("file has trailing bytes after its groups"); }

    const auto decode_group = [&](const StoredGroup &stored_group) {
        const std::span<const std::byte> stored = bytes.subspan(
            stored_group.payload_offset, stored_group.payload_size);
        const std::uint64_t expanded_size = checked_add(
            stored_group.raw_data_size,
            stored_group.array_count * kArrayHeaderBytes,
            "group expanded");
        std::vector<std::byte> raw;
        if (document.group_deflate) {
            raw = inflate_group(stored, expanded_size);
        } else {
            if (stored.size() != expanded_size) {
                invalid("uncompressed group size does not match its header");
            }
            raw.assign(stored.begin(), stored.end());
        }

        XGenGroup group{};
        group.index = stored_group.index;
        group.flags = stored_group.flags;
        if (stored_group.array_count > std::numeric_limits<std::size_t>::max()) {
            invalid("group array count exceeds size_t");
        }
        group.arrays.reserve(static_cast<std::size_t>(stored_group.array_count));
        std::size_t array_offset = 0u;
        std::uint64_t data_sum = 0u;
        for (std::uint64_t array_index = 0u;
             array_index < stored_group.array_count; ++array_index) {
            if (array_offset > raw.size() || raw.size() - array_offset < kArrayHeaderBytes) {
                invalid("array record header is truncated");
            }
            XGenArray array{};
            array.type_tag = read_u64(raw, array_offset);
            const std::uint64_t array_size = read_u64(raw, array_offset + 8u);
            array_offset += kArrayHeaderBytes;
            if (array_size > raw.size() - array_offset) {
                invalid("array record data is truncated");
            }
            const std::size_t size = static_cast<std::size_t>(array_size);
            array.bytes.assign(raw.begin() + static_cast<std::ptrdiff_t>(array_offset),
                               raw.begin() + static_cast<std::ptrdiff_t>(array_offset + size));
            array_offset += size;
            data_sum = checked_add(data_sum, array_size, "group raw data");
            group.arrays.emplace_back(std::move(array));
        }
        if (array_offset != raw.size() || data_sum != stored_group.raw_data_size) {
            invalid("group array sizes do not consume the declared payload");
        }
        return group;
    };

    // XGen commonly splits large snapshots into independently deflated groups.
    // Decode a bounded number in parallel; group placement stays deterministic.
    const std::size_t worker_count = std::min<std::size_t>(
        stored_groups.size(), std::max<std::size_t>(
            1u, std::min<std::size_t>(8u, std::thread::hardware_concurrency())));
    if (worker_count <= 1u) {
        for (const StoredGroup &stored_group : stored_groups) {
            document.groups[stored_group.index] = decode_group(stored_group);
        }
    } else {
        std::atomic_size_t next_group{0u};
        std::atomic_bool failed{false};
        std::exception_ptr first_error;
        std::mutex error_mutex;
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0u; worker < worker_count; ++worker) {
            workers.emplace_back([&] {
                while (!failed.load(std::memory_order_relaxed)) {
                    const std::size_t ordinal = next_group.fetch_add(
                        1u, std::memory_order_relaxed);
                    if (ordinal >= stored_groups.size()) { return; }
                    try {
                        const StoredGroup &stored_group = stored_groups[ordinal];
                        document.groups[stored_group.index] = decode_group(stored_group);
                    } catch (...) {
                        std::lock_guard lock{error_mutex};
                        if (!first_error) { first_error = std::current_exception(); }
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }
        workers.clear(); // join all jthreads before inspecting first_error
        if (first_error) { std::rethrow_exception(first_error); }
    }
    return document;
}

XGenDocument load_xgen_document(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) { throw std::runtime_error("failed to open XGen BLOB: " + path.string()); }
    const std::streamoff end = stream.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) >
                       std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("failed to query XGen BLOB size");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    if (bytes.size() > static_cast<std::size_t>(
                           std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("XGen BLOB exceeds streamsize");
    }
    stream.seekg(0);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char *>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) { throw std::runtime_error("failed to read XGen BLOB: " + path.string()); }
    return parse_xgen_document(bytes);
}

std::vector<std::byte> serialize_xgen_document(const XGenDocument &document) {
    const MetadataHeader metadata = parse_metadata(document.metadata_json);
    if (metadata.version != document.version ||
        metadata.group_version != document.group_version ||
        metadata.group_base64 != document.group_base64 ||
        metadata.group_deflate != document.group_deflate ||
        metadata.group_deflate_level != document.group_deflate_level ||
        metadata.group_count != document.groups.size()) {
        throw std::invalid_argument("XGen document fields disagree with metadata JSON");
    }
    if (document.group_base64) {
        throw std::invalid_argument("GroupBase64=true is not supported by the binary BLOB API");
    }
    std::vector<bool> seen_groups(document.groups.size(), false);
    std::vector<std::byte> output;
    append_u64(output, kXGenFileMagic);
    append_u64(output, document.metadata_json.size());
    append_bytes(output, std::as_bytes(std::span{document.metadata_json}));

    for (std::size_t ordinal = 0u; ordinal < document.groups.size(); ++ordinal) {
        const XGenGroup &group = document.groups[ordinal];
        if (group.index != ordinal || group.index >= document.groups.size() ||
            seen_groups[group.index]) {
            throw std::invalid_argument("XGen document group index is invalid or duplicated");
        }
        seen_groups[group.index] = true;
        std::vector<std::byte> raw;
        std::uint64_t raw_data_size = 0u;
        for (const XGenArray &array : group.arrays) {
            append_u64(raw, array.type_tag);
            append_u64(raw, array.bytes.size());
            append_bytes(raw, array.bytes);
            raw_data_size = checked_add(raw_data_size, array.bytes.size(), "group raw data");
        }
        std::vector<std::byte> stored = document.group_deflate
            ? deflate_group(raw, document.group_deflate_level) : raw;
        const std::uint64_t stored_size = checked_add(32u, stored.size(), "group stored");
        append_u64(output, kXGenGroupMagic);
        append_u64(output, stored_size);
        append_u64(output, group.index);
        append_u64(output, group.arrays.size());
        append_u64(output, raw_data_size);
        append_u64(output, group.flags);
        append_bytes(output, stored);
    }
    return output;
}

void save_xgen_document(
    const XGenDocument &document, const std::filesystem::path &path) {
    const std::vector<std::byte> bytes = serialize_xgen_document(document);
    std::ofstream stream(path, std::ios::binary);
    if (!stream) { throw std::runtime_error("failed to open XGen output: " + path.string()); }
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) { throw std::runtime_error("failed to write XGen output: " + path.string()); }
}

XGenEvaluatedCurves materialize_xgen_curves(
    const XGenDocument &document, XGenCurveOrder order) {
    const MetadataHeader metadata = parse_metadata(document.metadata_json);
    const std::vector<JsonValue> &items = as_array(member(metadata.root, "Items"), "Items");
    if (items.empty()) { invalid("metadata contains no Items"); }
    std::vector<MaterializedCurve> curves;
    for (const JsonValue &item : items) {
        const std::vector<std::uint32_t> primitive_infos = copy_typed_array<std::uint32_t>(
            document, item, "PrimitiveInfos", kXGenUInt32ArrayTag);
        const std::vector<Vec3> positions = copy_typed_array<Vec3>(
            document, item, "Positions", kXGenVec3ArrayTag);
        const std::vector<Vec2> patch_uvs = copy_typed_array<Vec2>(
            document, item, "PatchUVs", kXGenVec2ArrayTag);
        const std::vector<Vec2> face_uvs = copy_typed_array<Vec2>(
            document, item, "FaceUV", kXGenVec2ArrayTag);
        const std::vector<std::uint32_t> face_ids = copy_typed_array<std::uint32_t>(
            document, item, "FaceId", kXGenUInt32ArrayTag);
        const std::vector<float> widths = copy_typed_array<float>(
            document, item, "WIDTH_CV", kXGenFloatArrayTag);
        if (face_ids.empty() || face_uvs.size() != face_ids.size() ||
            primitive_infos.size() % face_ids.size() != 0u) {
            invalid("Item primitive metadata sizes do not agree");
        }
        const std::size_t stride = primitive_infos.size() / face_ids.size();
        if (stride < 2u || positions.size() != widths.size() ||
            patch_uvs.size() != positions.size()) {
            invalid("Item varying array sizes do not agree");
        }
        for (std::size_t primitive = 0u; primitive < face_ids.size(); ++primitive) {
            const std::uint32_t first = primitive_infos[primitive * stride];
            const std::uint32_t count = primitive_infos[primitive * stride + 1u];
            if (count < 2u || first > positions.size() ||
                count > positions.size() - first) {
                invalid("Item primitive range is out of bounds");
            }
            MaterializedCurve curve{};
            curve.face_id = face_ids[primitive];
            curve.face_uv = face_uvs[primitive];
            curve.patch_uv = patch_uvs[first];
            if (!finite(curve.face_uv) || !finite(curve.patch_uv)) {
                invalid("Item contains a non-finite curve UV");
            }
            curve.positions.reserve(count);
            curve.widths.reserve(count);
            curve.texcoords.reserve(count);
            for (std::uint32_t cv = 0u; cv < count; ++cv) {
                const std::size_t index = static_cast<std::size_t>(first) + cv;
                if (!finite(positions[index]) || !finite(patch_uvs[index]) ||
                    !std::isfinite(widths[index]) || widths[index] < 0.0f) {
                    invalid("Item contains an invalid varying value");
                }
                curve.positions.push_back(positions[index]);
                curve.widths.push_back(widths[index]);
                // XgFnSpline exposes the varying curve coordinate as (0, t).
                // PatchUVs is a separate per-CV channel (constant along each
                // curve in evaluated v1 BLOBs), not the renderer texcoord.
                curve.texcoords.push_back(
                    {0.0f, static_cast<float>(cv) / static_cast<float>(count - 1u)});
            }
            curves.emplace_back(std::move(curve));
        }
    }
    if (curves.empty()) { invalid("Items contain no curves"); }
    if (order == XGenCurveOrder::Canonical) {
        std::stable_sort(curves.begin(), curves.end(), [](const auto &a, const auto &b) {
            return curve_key(a) < curve_key(b);
        });
        for (std::size_t curve = 1u; curve < curves.size(); ++curve) {
            if (curve_key(curves[curve - 1u]) == curve_key(curves[curve])) {
                invalid("duplicate canonical curve identity");
            }
        }
    }

    XGenEvaluatedCurves result{};
    for (const MaterializedCurve &curve : curves) {
        if (curve.positions.size() > std::numeric_limits<std::uint32_t>::max()) {
            invalid("curve CV count exceeds uint32");
        }
        result.point_counts.push_back(static_cast<std::uint32_t>(curve.positions.size()));
        result.patch_uvs.push_back(curve.patch_uv);
        result.face_uvs.push_back(curve.face_uv);
        result.face_ids.push_back(curve.face_id);
        result.positions.insert(
            result.positions.end(), curve.positions.begin(), curve.positions.end());
        result.widths.insert(result.widths.end(), curve.widths.begin(), curve.widths.end());
        result.texcoords.insert(
            result.texcoords.end(), curve.texcoords.begin(), curve.texcoords.end());
    }
    return result;
}

XGenPackedCurves materialize_xgen_packed_curves(
    const XGenDocument &document, XGenCurveOrder order) {
    return pack_document_arrays(document, order);
}

XGenPackedCurves parse_xgen_packed_curves(
    std::span<const std::byte> bytes, XGenCurveOrder order) {
    return parse_fused_packed_curves(bytes, order);
}

XGenPackedCurves load_xgen_packed_curves(
    const std::filesystem::path &path, XGenCurveOrder order) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) { throw std::runtime_error("failed to open XGen BLOB: " + path.string()); }
    const std::streamoff end = stream.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) >
                       std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("failed to query XGen BLOB size");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    if (bytes.size() > static_cast<std::size_t>(
                           std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("XGen BLOB exceeds streamsize");
    }
    stream.seekg(0);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char *>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) { throw std::runtime_error("failed to read XGen BLOB: " + path.string()); }
    return parse_fused_packed_curves(bytes, order);
}

XGenEvaluatedCurves make_xgen_curves(const GeneratedCurves &curves) {
    const std::size_t expected_points =
        static_cast<std::size_t>(curves.strand_count) * curves.cvs_per_strand;
    if (curves.strand_count == 0u || curves.cvs_per_strand < 2u ||
        curves.roots.size() != curves.strand_count ||
        curves.points.size() != expected_points || curves.widths.size() != expected_points) {
        throw std::invalid_argument("generated curve arrays are incomplete");
    }
    XGenEvaluatedCurves result{};
    result.point_counts.assign(curves.strand_count, curves.cvs_per_strand);
    result.positions = curves.points;
    result.widths = curves.widths;
    result.texcoords.reserve(curves.points.size());
    result.patch_uvs.reserve(curves.strand_count);
    result.face_uvs.reserve(curves.strand_count);
    result.face_ids.reserve(curves.strand_count);
    for (std::uint32_t strand = 0u; strand < curves.strand_count; ++strand) {
        const RootSample &root = curves.roots[strand];
        result.patch_uvs.push_back(root.uv);
        result.face_uvs.push_back(root.barycentric);
        result.face_ids.push_back(root.triangle_index);
        for (std::uint32_t cv = 0u; cv < curves.cvs_per_strand; ++cv) {
            result.texcoords.push_back(
                {0.0f, static_cast<float>(cv) /
                           static_cast<float>(curves.cvs_per_strand - 1u)});
        }
    }
    validate_materialized(result);
    return result;
}

void process_xgen_curves(
    XGenEvaluatedCurves &curves, const XGenProcessParams &params) {
    validate_materialized(curves);
    if (!finite(params.translation) || !std::isfinite(params.length_scale) ||
        params.length_scale < 0.0f || !std::isfinite(params.width_scale) ||
        params.width_scale < 0.0f) {
        throw std::invalid_argument("XGen process parameters must be finite and non-negative");
    }
    const bool transform_positions = params.length_scale != 1.0f ||
        params.translation.x != 0.0f || params.translation.y != 0.0f ||
        params.translation.z != 0.0f;
    const bool transform_widths = params.width_scale != 1.0f;
    std::size_t first = 0u;
    for (const std::uint32_t count : curves.point_counts) {
        const Vec3 root = curves.positions[first];
        for (std::uint32_t cv = 0u; cv < count; ++cv) {
            const std::size_t point = first + cv;
            if (transform_positions) {
                curves.positions[point] = root +
                    (curves.positions[point] - root) * params.length_scale +
                    params.translation;
            }
            if (transform_widths) { curves.widths[point] *= params.width_scale; }
        }
        first += count;
    }
}

void process_xgen_document(
    XGenDocument &document, const XGenProcessParams &params) {
    if (!finite(params.translation) || !std::isfinite(params.length_scale) ||
        params.length_scale < 0.0f || !std::isfinite(params.width_scale) ||
        params.width_scale < 0.0f) {
        throw std::invalid_argument("XGen process parameters must be finite and non-negative");
    }
    const MetadataHeader metadata = parse_metadata(document.metadata_json);
    const std::vector<JsonValue> &items = as_array(member(metadata.root, "Items"), "Items");
    std::set<std::uint64_t> processed_positions;
    std::set<std::uint64_t> processed_widths;
    const bool transform_positions = params.length_scale != 1.0f ||
        params.translation.x != 0.0f || params.translation.y != 0.0f ||
        params.translation.z != 0.0f;
    const bool transform_widths = params.width_scale != 1.0f;
    for (const JsonValue &item : items) {
        const std::vector<std::uint32_t> primitive_infos = copy_typed_array<std::uint32_t>(
            document, item, "PrimitiveInfos", kXGenUInt32ArrayTag);
        const std::uint64_t positions_id = as_u64(member(item, "Positions"), "Positions");
        const std::uint64_t widths_id = as_u64(member(item, "WIDTH_CV"), "WIDTH_CV");
        std::vector<Vec3> positions = copy_typed_array<Vec3>(
            document, item, "Positions", kXGenVec3ArrayTag);
        std::vector<float> widths = copy_typed_array<float>(
            document, item, "WIDTH_CV", kXGenFloatArrayTag);
        const std::vector<std::uint32_t> face_ids = copy_typed_array<std::uint32_t>(
            document, item, "FaceId", kXGenUInt32ArrayTag);
        if (face_ids.empty() || primitive_infos.size() % face_ids.size() != 0u ||
            widths.size() != positions.size()) {
            invalid("Item arrays disagree while processing");
        }
        const std::size_t stride = primitive_infos.size() / face_ids.size();
        if (stride < 2u) { invalid("Item primitive info stride is too small"); }
        if (transform_positions && processed_positions.insert(positions_id).second) {
            for (std::size_t primitive = 0u; primitive < face_ids.size(); ++primitive) {
                const std::uint32_t first = primitive_infos[primitive * stride];
                const std::uint32_t count = primitive_infos[primitive * stride + 1u];
                if (count < 2u || first > positions.size() ||
                    count > positions.size() - first) {
                    invalid("Item primitive range is out of bounds while processing");
                }
                const Vec3 root = positions[first];
                for (std::uint32_t cv = 0u; cv < count; ++cv) {
                    Vec3 &point = positions[static_cast<std::size_t>(first) + cv];
                    point = root + (point - root) * params.length_scale + params.translation;
                }
            }
            XGenArray &array = array_by_id(document, positions_id);
            std::memcpy(array.bytes.data(), positions.data(), array.bytes.size());
        }
        if (transform_widths && processed_widths.insert(widths_id).second) {
            for (float &width : widths) { width *= params.width_scale; }
            XGenArray &array = array_by_id(document, widths_id);
            std::memcpy(array.bytes.data(), widths.data(), array.bytes.size());
        }
    }
}

XGenDocument build_xgen_document(
    const XGenEvaluatedCurves &curves, const XGenBuildOptions &options) {
    validate_materialized(curves);
    if (options.item_name.empty()) {
        throw std::invalid_argument("XGen item name must not be empty");
    }
    if (options.target_group_bytes == 0u || options.group_deflate_level > 9u) {
        throw std::invalid_argument("invalid XGen group build options");
    }
    if (curves.positions.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("XGen point count exceeds uint32 primitive offsets");
    }

    std::vector<std::uint32_t> primitive_infos;
    std::vector<Vec2> varying_patch_uvs;
    std::vector<Vec3> mesh_n;
    std::vector<Vec3> mesh_u;
    std::vector<Vec3> mesh_v;
    std::vector<float> frozen_set(curves.positions.size(), 0.0f);
    std::vector<Vec3> directions(curves.positions.size());
    primitive_infos.reserve(curves.point_counts.size() * 3u);
    varying_patch_uvs.reserve(curves.positions.size());
    mesh_n.reserve(curves.point_counts.size());
    mesh_u.reserve(curves.point_counts.size());
    mesh_v.reserve(curves.point_counts.size());
    std::size_t first = 0u;
    for (std::size_t curve = 0u; curve < curves.point_counts.size(); ++curve) {
        const std::uint32_t count = curves.point_counts[curve];
        primitive_infos.push_back(static_cast<std::uint32_t>(first));
        primitive_infos.push_back(count);
        primitive_infos.push_back(0u);
        for (std::uint32_t cv = 0u; cv < count; ++cv) {
            varying_patch_uvs.push_back(curves.patch_uvs[curve]);
            Vec3 tangent{};
            if (cv == 0u) {
                tangent = curves.positions[first + 1u] - curves.positions[first];
            } else if (cv + 1u == count) {
                tangent = curves.positions[first + cv] - curves.positions[first + cv - 1u];
            } else {
                tangent = curves.positions[first + cv + 1u] -
                    curves.positions[first + cv - 1u];
            }
            directions[first + cv] = normalize_or(tangent, {0.0f, 1.0f, 0.0f});
        }
        const Vec3 normal = directions[first];
        const Vec3 helper = std::abs(normal.y) < 0.9f
            ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 u = normalize_or(cross(helper, normal), {1.0f, 0.0f, 0.0f});
        mesh_n.push_back(normal);
        mesh_u.push_back(u);
        mesh_v.push_back(cross(normal, u));
        first += count;
    }

    struct NamedArray {
        const char *name;
        XGenArray array;
        std::uint64_t id{};
    };
    std::vector<NamedArray> arrays;
    arrays.reserve(11u);
    arrays.push_back({"PrimitiveInfos", make_array<std::uint32_t>(
        kXGenUInt32ArrayTag, primitive_infos)});
    arrays.push_back({"Positions", make_array<Vec3>(kXGenVec3ArrayTag, curves.positions)});
    arrays.push_back({"PatchUVs", make_array<Vec2>(kXGenVec2ArrayTag, varying_patch_uvs)});
    arrays.push_back({"MeshN", make_array<Vec3>(kXGenVec3ArrayTag, mesh_n)});
    arrays.push_back({"MeshU", make_array<Vec3>(kXGenVec3ArrayTag, mesh_u)});
    arrays.push_back({"MeshV", make_array<Vec3>(kXGenVec3ArrayTag, mesh_v)});
    arrays.push_back({"FaceUV", make_array<Vec2>(kXGenVec2ArrayTag, curves.face_uvs)});
    arrays.push_back({"FaceId", make_array<std::uint32_t>(
        kXGenUInt32ArrayTag, curves.face_ids)});
    arrays.push_back({"FrozenSet", make_array<float>(kXGenFloatArrayTag, frozen_set)});
    arrays.push_back({"WIDTH_CV", make_array<float>(kXGenFloatArrayTag, curves.widths)});
    arrays.push_back({"DIRECTION_CV", make_array<Vec3>(kXGenVec3ArrayTag, directions)});

    XGenDocument document{};
    document.version = 1u;
    document.group_version = 1u;
    document.group_base64 = false;
    document.group_deflate = true;
    document.group_deflate_level = options.group_deflate_level;
    std::uint64_t group_bytes = 0u;
    for (NamedArray &named : arrays) {
        const std::uint64_t record_bytes = checked_add(
            kArrayHeaderBytes, named.array.bytes.size(), "array record");
        if (document.groups.empty() ||
            (!document.groups.back().arrays.empty() &&
             group_bytes > options.target_group_bytes -
                 std::min(options.target_group_bytes, record_bytes))) {
            if (document.groups.size() >= std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error("XGen group count exceeds uint32");
            }
            document.groups.push_back(
                {static_cast<std::uint32_t>(document.groups.size()), 0u, {}});
            group_bytes = 0u;
        }
        XGenGroup &group = document.groups.back();
        if (group.arrays.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("XGen array index exceeds uint32");
        }
        named.id = (static_cast<std::uint64_t>(group.index) << 32u) |
            static_cast<std::uint64_t>(group.arrays.size());
        group.arrays.push_back(std::move(named.array));
        group_bytes = checked_add(group_bytes, record_bytes, "group raw");
    }

    std::ostringstream metadata;
    metadata << "{\n  \"Header\":{\"Version\":1,\"Type\":\"XgSplineData\","
             << "\"GroupVersion\":1,\"GroupCount\":" << document.groups.size()
             << ",\"GroupBase64\":false"
             << ",\"GroupDeflate\":true"
             << ",\"GroupDeflateLevel\":" << options.group_deflate_level << "},\n"
             << "  \"Items\":[{\"Name\":\"" << json_escape(options.item_name)
             << "\",\"Id\":\"00000000-0000-0000-0000-000000000001\","
             << "\"Mode\":\"Density\"";
    for (const NamedArray &named : arrays) {
        metadata << ",\"" << named.name << "\":" << named.id;
    }
    metadata << "}],\n  \"RefMeshArray\":[],\n  \"CustomData\":{}\n}";
    document.metadata_json = metadata.str();
    return document;
}

} // namespace nanoxgen
