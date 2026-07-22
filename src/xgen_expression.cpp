#include "nanoxgen/xgen_expression.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace nanoxgen {
namespace {

constexpr std::array<std::uint8_t, 256u> permutation{
    148,201,203,34,85,225,163,200,174,137,51,24,19,252,107,173,110,251,149,69,180,152,
    141,132,22,20,147,219,37,46,154,114,59,49,155,161,239,77,47,10,70,227,53,235,
    30,188,143,73,88,193,214,194,18,120,176,36,212,84,211,142,167,57,153,71,159,151,
    126,115,229,124,172,101,79,183,32,38,68,11,67,109,221,3,4,61,122,94,72,117,
    12,240,199,76,118,5,48,197,128,62,119,89,14,45,226,195,80,50,40,192,60,65,
    166,106,90,215,213,232,250,207,104,52,182,29,157,103,242,97,111,17,8,175,254,108,
    208,224,191,112,105,187,43,56,185,243,196,156,246,249,184,7,135,6,158,82,130,234,
    206,255,160,236,171,230,42,98,54,74,209,205,33,177,15,138,178,44,116,96,140,253,
    233,125,21,133,136,86,245,58,23,1,75,165,92,217,39,0,218,91,179,55,238,170,
    134,83,25,189,216,100,129,150,241,210,123,99,2,164,16,220,121,139,168,64,190,9,
    31,228,95,247,244,81,102,145,204,146,26,87,113,198,181,127,237,169,28,93,27,41,
    231,248,78,162,13,186,63,66,131,202,35,144,222,223};

enum class TokenKind : std::uint8_t {
    end, number, variable, identifier, string_literal, semicolon, comma,
    left_paren, right_paren, question, colon, assign, plus, minus, star,
    slash, bang, less, less_equal, greater, greater_equal, equal, not_equal,
    logical_and, logical_or,
};

struct Token {
    TokenKind kind{TokenKind::end};
    std::string text;
    double number{};
    std::size_t offset{};
};

[[noreturn]] void syntax_error(std::size_t offset, const std::string &message) {
    throw std::runtime_error(
        "XGen scalar expression error at byte " + std::to_string(offset) + ": " + message);
}

std::string normalize_source(std::string_view source, std::size_t limit) {
    if (source.size() > limit) {
        throw std::runtime_error("XGen scalar expression source byte limit exceeded");
    }
    if (source.find('\0') != std::string_view::npos) {
        throw std::runtime_error("XGen scalar expression contains a NUL byte");
    }
    std::string result;
    result.reserve(source.size());
    bool comment = false;
    for (std::size_t i = 0u; i < source.size(); ++i) {
        char c = source[i];
        if (c == '\\' && i + 1u < source.size() && source[i + 1u] == 'n') {
            c = '\n';
            ++i;
        }
        if (comment) {
            if (c == '\n' || c == '\r') {
                comment = false;
                result.push_back(';');
            }
            continue;
        }
        if (c == '#') {
            comment = true;
        } else if (c == '\n' || c == '\r') {
            result.push_back(';');
        } else {
            result.push_back(c);
        }
    }
    return result;
}

bool identifier_start(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool identifier_continue(char c) noexcept {
    return identifier_start(c) || (c >= '0' && c <= '9');
}

std::vector<Token> tokenize(std::string_view source, std::size_t limit) {
    std::vector<Token> result;
    auto push = [&](Token token) {
        if (result.size() >= limit) {
            throw std::runtime_error("XGen scalar expression token limit exceeded");
        }
        result.emplace_back(std::move(token));
    };
    for (std::size_t i = 0u; i < source.size();) {
        const char c = source[i];
        if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
            ++i;
            continue;
        }
        if ((c >= '0' && c <= '9') ||
            (c == '.' && i + 1u < source.size() && source[i + 1u] >= '0' &&
             source[i + 1u] <= '9')) {
            const char *begin = source.data() + i;
            char *end = nullptr;
            const double value = std::strtod(begin, &end);
            const std::size_t consumed = static_cast<std::size_t>(end - begin);
            if (consumed == 0u || !std::isfinite(value)) {
                syntax_error(i, "invalid or non-finite numeric literal");
            }
            push({TokenKind::number, {}, value, i});
            i += consumed;
            continue;
        }
        if (c == '$') {
            const std::size_t start = i++;
            if (i >= source.size() || !identifier_start(source[i])) {
                syntax_error(start, "expected a variable name after '$'");
            }
            const std::size_t name = i++;
            while (i < source.size() && identifier_continue(source[i])) { ++i; }
            push({TokenKind::variable, std::string{source.substr(name, i - name)}, 0.0, start});
            continue;
        }
        if (identifier_start(c)) {
            const std::size_t start = i++;
            while (i < source.size() && identifier_continue(source[i])) { ++i; }
            push({TokenKind::identifier, std::string{source.substr(start, i - start)}, 0.0, start});
            continue;
        }
        if (c == '\'' || c == '"') {
            const std::size_t start = i++;
            const char quote = c;
            std::string text;
            while (i < source.size() && source[i] != quote) {
                if (source[i] == '\\' && i + 1u < source.size()) { ++i; }
                text.push_back(source[i++]);
            }
            if (i == source.size()) { syntax_error(start, "unterminated string literal"); }
            ++i;
            push({TokenKind::string_literal, std::move(text), 0.0, start});
            continue;
        }
        auto two = [&](char second, TokenKind yes, TokenKind no) {
            if (i + 1u < source.size() && source[i + 1u] == second) {
                push({yes, {}, 0.0, i});
                i += 2u;
            } else {
                push({no, {}, 0.0, i++});
            }
        };
        switch (c) {
        case ';': push({TokenKind::semicolon, {}, 0.0, i++}); break;
        case ',': push({TokenKind::comma, {}, 0.0, i++}); break;
        case '(': push({TokenKind::left_paren, {}, 0.0, i++}); break;
        case ')': push({TokenKind::right_paren, {}, 0.0, i++}); break;
        case '?': push({TokenKind::question, {}, 0.0, i++}); break;
        case ':': push({TokenKind::colon, {}, 0.0, i++}); break;
        case '+': push({TokenKind::plus, {}, 0.0, i++}); break;
        case '-': push({TokenKind::minus, {}, 0.0, i++}); break;
        case '*': push({TokenKind::star, {}, 0.0, i++}); break;
        case '/': push({TokenKind::slash, {}, 0.0, i++}); break;
        case '=': two('=', TokenKind::equal, TokenKind::assign); break;
        case '!': two('=', TokenKind::not_equal, TokenKind::bang); break;
        case '<': two('=', TokenKind::less_equal, TokenKind::less); break;
        case '>': two('=', TokenKind::greater_equal, TokenKind::greater); break;
        case '&':
            if (i + 1u >= source.size() || source[i + 1u] != '&') {
                syntax_error(i, "single '&' is unsupported; use '&&'");
            }
            push({TokenKind::logical_and, {}, 0.0, i}); i += 2u; break;
        case '|':
            if (i + 1u >= source.size() || source[i + 1u] != '|') {
                syntax_error(i, "single '|' is unsupported; use '||'");
            }
            push({TokenKind::logical_or, {}, 0.0, i}); i += 2u; break;
        default: syntax_error(i, "unsupported character");
        }
    }
    push({TokenKind::end, {}, 0.0, source.size()});
    return result;
}

class Compiler {
public:
    Compiler(std::vector<Token> tokens, const XgenExpressionCompileOptions &options)
        : _tokens{std::move(tokens)}, _options{options} {
        _program.expression_seed = xgen_expression_seed(
            options.expression_name, options.object_type);
    }

    XgenExpressionProgram compile() {
        bool have_result = false;
        while (peek().kind != TokenKind::end) {
            while (accept(TokenKind::semicolon)) {}
            if (peek().kind == TokenKind::end) { break; }
            if (peek().kind == TokenKind::variable &&
                peek(1u).kind == TokenKind::assign) {
                const Token variable = take();
                take();
                const std::uint32_t value = expression();
                if (!_locals.contains(variable.text) &&
                    _locals.size() >= _options.max_locals) {
                    throw std::runtime_error("XGen scalar expression local limit exceeded");
                }
                _locals[variable.text] = value;
                _program.result = value;
            } else {
                _program.result = expression();
            }
            have_result = true;
            if (peek().kind != TokenKind::semicolon && peek().kind != TokenKind::end) {
                syntax_error(peek().offset, "expected ';' after expression");
            }
        }
        if (!have_result) { syntax_error(0u, "expression is empty"); }
        return std::move(_program);
    }

private:
    const Token &peek(std::size_t lookahead = 0u) const {
        return _tokens[std::min(_cursor + lookahead, _tokens.size() - 1u)];
    }
    Token take() { return _tokens[_cursor++]; }
    bool accept(TokenKind kind) {
        if (peek().kind != kind) { return false; }
        ++_cursor;
        return true;
    }
    void expect(TokenKind kind, const char *message) {
        if (!accept(kind)) { syntax_error(peek().offset, message); }
    }

    std::uint32_t emit(XgenScalarOp op, std::span<const std::uint32_t> operands = {},
                       double immediate = 0.0, std::uint32_t auxiliary = 0u) {
        if (_program.instructions.size() >= _options.max_instructions) {
            throw std::runtime_error("XGen scalar expression instruction limit exceeded");
        }
        if (operands.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("XGen scalar expression operand limit exceeded");
        }
        const std::size_t offset = _program.operands.size();
        if (offset > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("XGen scalar expression operand storage exceeded");
        }
        for (const std::uint32_t operand : operands) {
            if (operand >= _program.instructions.size()) {
                throw std::logic_error("internal expression compiler SSA ordering error");
            }
            _program.operands.push_back(operand);
        }
        _program.instructions.push_back({op, static_cast<std::uint32_t>(offset),
            static_cast<std::uint16_t>(operands.size()), auxiliary, immediate});
        return static_cast<std::uint32_t>(_program.instructions.size() - 1u);
    }

    std::uint32_t unary(XgenScalarOp op, std::uint32_t value) {
        const std::array operands{value};
        return emit(op, operands);
    }
    std::uint32_t binary(XgenScalarOp op, std::uint32_t lhs, std::uint32_t rhs) {
        const std::array operands{lhs, rhs};
        return emit(op, operands);
    }

    std::uint32_t expression() { return ternary(); }
    std::uint32_t ternary() {
        std::uint32_t condition = logical_or();
        if (!accept(TokenKind::question)) { return condition; }
        const std::uint32_t yes = expression();
        expect(TokenKind::colon, "expected ':' in conditional expression");
        const std::uint32_t no = ternary();
        const std::array operands{condition, yes, no};
        return emit(XgenScalarOp::select, operands);
    }
    std::uint32_t logical_or() {
        std::uint32_t value = logical_and();
        while (accept(TokenKind::logical_or)) {
            value = binary(XgenScalarOp::logical_or, value, logical_and());
        }
        return value;
    }
    std::uint32_t logical_and() {
        std::uint32_t value = equality();
        while (accept(TokenKind::logical_and)) {
            value = binary(XgenScalarOp::logical_and, value, equality());
        }
        return value;
    }
    std::uint32_t equality() {
        std::uint32_t value = comparison();
        for (;;) {
            if (accept(TokenKind::equal)) {
                value = binary(XgenScalarOp::equal, value, comparison());
            } else if (accept(TokenKind::not_equal)) {
                value = binary(XgenScalarOp::not_equal, value, comparison());
            } else { return value; }
        }
    }
    std::uint32_t comparison() {
        std::uint32_t value = additive();
        for (;;) {
            if (accept(TokenKind::less)) {
                value = binary(XgenScalarOp::less, value, additive());
            } else if (accept(TokenKind::less_equal)) {
                value = binary(XgenScalarOp::less_equal, value, additive());
            } else if (accept(TokenKind::greater)) {
                value = binary(XgenScalarOp::greater, value, additive());
            } else if (accept(TokenKind::greater_equal)) {
                value = binary(XgenScalarOp::greater_equal, value, additive());
            } else { return value; }
        }
    }
    std::uint32_t additive() {
        std::uint32_t value = multiplicative();
        for (;;) {
            if (accept(TokenKind::plus)) {
                value = binary(XgenScalarOp::add, value, multiplicative());
            } else if (accept(TokenKind::minus)) {
                value = binary(XgenScalarOp::subtract, value, multiplicative());
            } else { return value; }
        }
    }
    std::uint32_t multiplicative() {
        std::uint32_t value = prefix();
        for (;;) {
            if (accept(TokenKind::star)) {
                value = binary(XgenScalarOp::multiply, value, prefix());
            } else if (accept(TokenKind::slash)) {
                value = binary(XgenScalarOp::divide, value, prefix());
            } else { return value; }
        }
    }
    std::uint32_t prefix() {
        if (accept(TokenKind::plus)) { return prefix(); }
        if (accept(TokenKind::minus)) { return unary(XgenScalarOp::negate, prefix()); }
        if (accept(TokenKind::bang)) { return unary(XgenScalarOp::logical_not, prefix()); }
        return primary();
    }

    double ramp_number(const char *what) {
        double sign = 1.0;
        if (accept(TokenKind::plus)) {
        } else if (accept(TokenKind::minus)) {
            sign = -1.0;
        }
        if (peek().kind != TokenKind::number) {
            syntax_error(peek().offset, std::string{"expected a numeric ramp "} + what);
        }
        return sign * take().number;
    }

    std::uint32_t ramp(const Token &name) {
        const std::size_t point_offset = _program.ramp_points.size();
        for (;;) {
            const double position = ramp_number("position");
            expect(TokenKind::comma, "expected ',' after ramp position");
            const double value = ramp_number("value");
            expect(TokenKind::comma, "expected ',' after ramp value");
            const double interpolation = ramp_number("interpolation");
            if (interpolation != std::floor(interpolation) ||
                interpolation < 0.0 || interpolation > 3.0) {
                syntax_error(name.offset,
                    "ramp interpolation must be an integer in [0, 3]");
            }
            if (position < 0.0 || position > 1.0) {
                syntax_error(name.offset, "ramp positions must be in [0, 1]");
            }
            if (_program.ramp_points.size() >= _options.max_ramp_points) {
                throw std::runtime_error(
                    "XGen scalar expression ramp point limit exceeded");
            }
            if (_program.ramp_points.size() != point_offset &&
                position <= _program.ramp_points.back().position) {
                syntax_error(name.offset,
                    "ramp positions must be strictly increasing");
            }
            _program.ramp_points.push_back({
                position, value, static_cast<std::uint8_t>(interpolation)});
            if (!accept(TokenKind::colon)) { break; }
        }
        expect(TokenKind::right_paren, "expected ')' after rampUI points");
        const std::size_t point_count = _program.ramp_points.size() - point_offset;
        if (point_count == 0u ||
            point_count > std::numeric_limits<std::uint16_t>::max() ||
            point_offset > std::numeric_limits<std::uint32_t>::max()) {
            syntax_error(name.offset, "ramp point storage limit exceeded");
        }
        if (_program.ramps.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("XGen scalar expression ramp limit exceeded");
        }
        const std::uint32_t ramp_index =
            static_cast<std::uint32_t>(_program.ramps.size());
        _program.ramps.push_back({static_cast<std::uint32_t>(point_offset),
                                  static_cast<std::uint16_t>(point_count)});
        return emit(XgenScalarOp::ramp, {}, 0.0, ramp_index);
    }

    std::uint32_t primary() {
        if (peek().kind == TokenKind::number) {
            return emit(XgenScalarOp::constant, {}, take().number);
        }
        if (peek().kind == TokenKind::variable) {
            const Token variable = take();
            if (const auto found = _locals.find(variable.text); found != _locals.end()) {
                return found->second;
            }
            auto found = std::find(
                _program.inputs.begin(), _program.inputs.end(), variable.text);
            std::uint32_t index{};
            if (found == _program.inputs.end()) {
                _program.inputs.emplace_back(variable.text);
                index = static_cast<std::uint32_t>(_program.inputs.size() - 1u);
            } else {
                index = static_cast<std::uint32_t>(found - _program.inputs.begin());
            }
            return emit(XgenScalarOp::input, {}, 0.0, index);
        }
        if (accept(TokenKind::left_paren)) {
            const std::uint32_t value = expression();
            expect(TokenKind::right_paren, "expected ')'");
            return value;
        }
        if (peek().kind == TokenKind::string_literal) {
            syntax_error(peek().offset,
                "string values require an explicit PTEX binding such as map()");
        }
        if (peek().kind != TokenKind::identifier) {
            syntax_error(peek().offset, "expected a scalar value");
        }
        const Token name = take();
        if (name.text == "true") { return emit(XgenScalarOp::constant, {}, 1.0); }
        if (name.text == "false") { return emit(XgenScalarOp::constant, {}, 0.0); }
        if (!accept(TokenKind::left_paren)) {
            syntax_error(name.offset, "bare identifier is unsupported: " + name.text);
        }
        if (name.text == "rampUI") { return ramp(name); }
        std::vector<std::uint32_t> arguments;
        if (!accept(TokenKind::right_paren)) {
            do {
                arguments.push_back(expression());
            } while (accept(TokenKind::comma));
            expect(TokenKind::right_paren, "expected ')' after function arguments");
        }
        return call(name, arguments);
    }

    std::uint32_t call(const Token &name, const std::vector<std::uint32_t> &arguments) {
        auto arity = [&](std::size_t expected) {
            if (arguments.size() != expected) {
                syntax_error(name.offset, name.text + " expects " +
                    std::to_string(expected) + " argument(s)");
            }
        };
        if (name.text == "hash") {
            if (arguments.empty()) { syntax_error(name.offset, "hash expects at least one argument"); }
            return emit(XgenScalarOp::hash, arguments);
        }
        if (name.text == "rand") {
            if (arguments.size() > 3u) {
                syntax_error(name.offset, "rand expects zero to three arguments");
            }
            const std::array seed_arguments{
                static_cast<double>(_program.expression_seed),
                static_cast<double>(_program.random_call_count)};
            const double site_seed = xgen_seexpr_hash(seed_arguments);
            ++_program.random_call_count;
            return emit(XgenScalarOp::random, arguments, site_seed);
        }
        if (name.text == "min") { arity(2u); return emit(XgenScalarOp::minimum, arguments); }
        if (name.text == "max") { arity(2u); return emit(XgenScalarOp::maximum, arguments); }
        if (name.text == "clamp") { arity(3u); return emit(XgenScalarOp::clamp, arguments); }
        if (name.text == "fit") { arity(5u); return emit(XgenScalarOp::fit, arguments); }
        if (name.text == "map") {
            syntax_error(name.offset, "map requires an explicit PTEX binding and is not a scalar intrinsic");
        }
        syntax_error(name.offset, "unsupported function: " + name.text);
    }

    std::vector<Token> _tokens;
    const XgenExpressionCompileOptions &_options;
    std::size_t _cursor{};
    std::unordered_map<std::string, std::uint32_t> _locals;
    XgenExpressionProgram _program;
};

template<typename Scalar, typename Point>
Scalar evaluate_ramp_points(std::span<const Point> points, Scalar parameter) {
    if (points.empty()) {
        throw std::runtime_error("XGen expression ramp has no points");
    }
    if (parameter <= static_cast<Scalar>(points.front().position)) {
        return std::clamp(static_cast<Scalar>(points.front().value),
                          Scalar{0}, Scalar{1});
    }
    if (parameter >= static_cast<Scalar>(points.back().position)) {
        return std::clamp(static_cast<Scalar>(points.back().value),
                          Scalar{0}, Scalar{1});
    }
    std::size_t upper = 1u;
    while (upper < points.size() &&
           parameter >= static_cast<Scalar>(points[upper].position)) {
        ++upper;
    }
    const std::size_t lower = upper - 1u;
    const Scalar p0 = static_cast<Scalar>(points[lower].position);
    const Scalar p1 = static_cast<Scalar>(points[upper].position);
    const Scalar k0 = static_cast<Scalar>(points[lower].value);
    const Scalar k1 = static_cast<Scalar>(points[upper].value);
    const Scalar u = (parameter - p0) / (p1 - p0);
    Scalar value{};
    switch (points[lower].interpolation) {
    case 0u: value = k0; break;
    case 1u: value = k0 + u * (k1 - k0); break;
    case 2u:
        value = k0 * (u - Scalar{1}) * (u - Scalar{1}) *
                    (Scalar{2} * u + Scalar{1}) +
                k1 * u * u * (Scalar{3} - Scalar{2} * u);
        break;
    case 3u: {
        const Scalar scale = Scalar{1} / (p1 - p0);
        const Scalar d0 = lower == 0u
            ? (k1 - k0) / (scale * (Scalar{2} * p1 - Scalar{2} * p0))
            : Scalar{0.5} *
                  ((k1 - static_cast<Scalar>(points[lower - 1u].value)) /
                   (scale * (p1 -
                       static_cast<Scalar>(points[lower - 1u].position))));
        const Scalar d1 = upper + 1u == points.size()
            ? (k1 - k0) / (scale * (Scalar{2} * p1 - Scalar{2} * p0))
            : Scalar{0.5} *
                  ((static_cast<Scalar>(points[upper + 1u].value) - k0) /
                   (scale *
                    (static_cast<Scalar>(points[upper + 1u].position) - p0)));
        value = k0 * (u - Scalar{1}) * (u - Scalar{1}) *
                    (Scalar{2} * u + Scalar{1}) +
                k1 * u * u * (Scalar{3} - Scalar{2} * u) +
                d0 * u * (u - Scalar{1}) * (u - Scalar{1}) +
                d1 * u * u * (u - Scalar{1});
        break;
    }
    default:
        throw std::runtime_error("XGen expression ramp interpolation is invalid");
    }
    return std::clamp(value, Scalar{0}, Scalar{1});
}

} // namespace

double xgen_seexpr_hash(std::span<const double> arguments) noexcept {
    std::uint32_t seed = 0u;
    for (const double argument : arguments) {
        int exponent = 0;
        const double fraction = std::frexp(argument * 8.539734222673566, &exponent);
        const std::uint32_t component =
            static_cast<std::uint32_t>(fraction * 4294967295.0) ^
            static_cast<std::uint32_t>(exponent);
        seed = seed * 1664525u + component + 1013904223u;
    }
    seed ^= seed >> 11u;
    seed ^= (seed << 7u) & 0x9d2c5680u;
    seed ^= (seed << 15u) & 0xefc60000u;
    seed ^= seed >> 18u;
    const std::uint8_t b3 = permutation[seed & 0xffu];
    const std::uint8_t b2 = permutation[((seed >> 8u) + b3) & 0xffu];
    const std::uint8_t b1 = permutation[((seed >> 16u) + b2) & 0xffu];
    const std::uint8_t b0 = permutation[((seed >> 24u) + b1) & 0xffu];
    const std::uint32_t result = static_cast<std::uint32_t>(b0) |
        (static_cast<std::uint32_t>(b1) << 8u) |
        (static_cast<std::uint32_t>(b2) << 16u) |
        (static_cast<std::uint32_t>(b3) << 24u);
    return static_cast<double>(result) * (1.0 / 4294967295.0);
}

std::uint32_t xgen_runtime_hash32(std::span<const float> arguments) noexcept {
    std::uint32_t state = 0x9e3779b9u;
    for (const float argument : arguments) {
        state ^= std::bit_cast<std::uint32_t>(argument) + 0x9e3779b9u +
                 (state << 6u) + (state >> 2u);
        state ^= state >> 16u;
        state *= 0x7feb352du;
        state ^= state >> 15u;
        state *= 0x846ca68bu;
        state ^= state >> 16u;
    }
    return state;
}

float xgen_runtime_hash(std::span<const float> arguments) noexcept {
    return static_cast<float>(xgen_runtime_hash32(arguments) >> 8u) *
           (1.0f / 16777216.0f);
}

float xgen_runtime_face_seed(std::uint32_t description_id,
                             std::string_view patch_name,
                             std::uint32_t face_id) noexcept {
    if (const std::size_t colon = patch_name.rfind(':');
        colon != std::string_view::npos) {
        patch_name.remove_prefix(colon + 1u);
    }
    const std::array arguments{
        static_cast<float>(description_id),
        static_cast<float>(xgen_string_seed(patch_name)),
        static_cast<float>(face_id)};
    return xgen_runtime_hash(arguments);
}

std::uint32_t xgen_string_seed(std::string_view text) noexcept {
    std::uint32_t hash = 0u;
    for (const unsigned char c : text) {
        hash = (hash << 4u) + c;
        const std::uint32_t high = hash & 0xf0000000u;
        if (high != 0u) {
            hash ^= high >> 24u;
            hash &= ~high;
        }
    }
    return hash % 211u;
}

std::uint32_t xgen_expression_seed(
    std::string_view expression_name, std::string_view object_type) noexcept {
    std::string key;
    key.reserve(expression_name.size() + object_type.size());
    key.append(expression_name);
    key.append(object_type);
    return xgen_string_seed(key);
}

double xgen_face_seed(std::uint32_t description_id, std::string_view patch_name,
                      std::uint32_t face_id) noexcept {
    if (const std::size_t colon = patch_name.rfind(':'); colon != std::string_view::npos) {
        patch_name.remove_prefix(colon + 1u);
    }
    const std::array arguments{static_cast<double>(description_id),
        static_cast<double>(xgen_string_seed(patch_name)),
        static_cast<double>(face_id)};
    return xgen_seexpr_hash(arguments);
}

XgenExpressionProgram compile_xgen_scalar_expression(
    std::string_view source, const XgenExpressionCompileOptions &options) {
    if (options.max_source_bytes == 0u || options.max_tokens == 0u ||
        options.max_instructions == 0u || options.max_locals == 0u ||
        options.max_ramp_points == 0u) {
        throw std::runtime_error("XGen scalar expression limits must be nonzero");
    }
    const std::string normalized = normalize_source(source, options.max_source_bytes);
    Compiler compiler{tokenize(normalized, options.max_tokens), options};
    return compiler.compile();
}

XgenFloatExpressionProgram make_xgen_float_expression_program(
    const XgenExpressionProgram &program) {
    XgenFloatExpressionProgram result{};
    result.instructions.reserve(program.instructions.size());
    for (const XgenScalarInstruction &instruction : program.instructions) {
        const float immediate = static_cast<float>(instruction.immediate);
        if (!std::isfinite(immediate)) {
            throw std::runtime_error(
                "XGen expression immediate cannot be represented as float");
        }
        result.instructions.push_back({
            instruction.op, instruction.operand_offset,
            instruction.operand_count, instruction.auxiliary, immediate});
    }
    result.operands = program.operands;
    result.inputs = program.inputs;
    result.ramp_points.reserve(program.ramp_points.size());
    for (const XgenRampPoint &point : program.ramp_points) {
        const float position = static_cast<float>(point.position);
        const float value = static_cast<float>(point.value);
        if (!std::isfinite(position) || !std::isfinite(value)) {
            throw std::runtime_error(
                "XGen ramp point cannot be represented as float");
        }
        result.ramp_points.push_back({position, value, point.interpolation});
    }
    result.ramps = program.ramps;
    result.result = program.result;
    result.expression_seed = program.expression_seed;
    result.random_call_count = program.random_call_count;
    return result;
}

double evaluate_xgen_scalar_expression(
    const XgenExpressionProgram &program, const XgenExpressionContext &context) {
    if (context.inputs.size() != program.inputs.size()) {
        throw std::runtime_error("XGen scalar expression input count mismatch");
    }
    if (!std::isfinite(context.u) || !std::isfinite(context.v) ||
        !std::isfinite(context.face_seed) || !std::isfinite(context.t)) {
        throw std::runtime_error("XGen scalar expression context is non-finite");
    }
    for (const double input : context.inputs) {
        if (!std::isfinite(input)) {
            throw std::runtime_error("XGen scalar expression input is non-finite");
        }
    }
    std::vector<double> values;
    values.reserve(program.instructions.size());
    for (const XgenScalarInstruction &instruction : program.instructions) {
        const std::size_t end = static_cast<std::size_t>(instruction.operand_offset) +
                                instruction.operand_count;
        if (end > program.operands.size()) {
            throw std::runtime_error("XGen scalar expression has an invalid operand range");
        }
        auto operand = [&](std::size_t index) -> double {
            if (index >= instruction.operand_count) {
                throw std::runtime_error("XGen scalar expression instruction arity mismatch");
            }
            const std::uint32_t value = program.operands[instruction.operand_offset + index];
            if (value >= values.size()) {
                throw std::runtime_error("XGen scalar expression violates SSA ordering");
            }
            return values[value];
        };
        auto require_arity = [&](std::size_t count) {
            if (instruction.operand_count != count) {
                throw std::runtime_error("XGen scalar expression instruction arity mismatch");
            }
        };
        double value{};
        switch (instruction.op) {
        case XgenScalarOp::constant: require_arity(0u); value = instruction.immediate; break;
        case XgenScalarOp::input:
            require_arity(0u);
            if (instruction.auxiliary >= context.inputs.size()) {
                throw std::runtime_error("XGen scalar expression input index is invalid");
            }
            value = context.inputs[instruction.auxiliary];
            break;
        case XgenScalarOp::negate: require_arity(1u); value = -operand(0u); break;
        case XgenScalarOp::logical_not: require_arity(1u); value = operand(0u) == 0.0; break;
        case XgenScalarOp::add: require_arity(2u); value = operand(0u) + operand(1u); break;
        case XgenScalarOp::subtract: require_arity(2u); value = operand(0u) - operand(1u); break;
        case XgenScalarOp::multiply: require_arity(2u); value = operand(0u) * operand(1u); break;
        case XgenScalarOp::divide: require_arity(2u); value = operand(0u) / operand(1u); break;
        case XgenScalarOp::less: require_arity(2u); value = operand(0u) < operand(1u); break;
        case XgenScalarOp::less_equal: require_arity(2u); value = operand(0u) <= operand(1u); break;
        case XgenScalarOp::greater: require_arity(2u); value = operand(0u) > operand(1u); break;
        case XgenScalarOp::greater_equal: require_arity(2u); value = operand(0u) >= operand(1u); break;
        case XgenScalarOp::equal: require_arity(2u); value = operand(0u) == operand(1u); break;
        case XgenScalarOp::not_equal: require_arity(2u); value = operand(0u) != operand(1u); break;
        case XgenScalarOp::logical_and: require_arity(2u); value = operand(0u) != 0.0 && operand(1u) != 0.0; break;
        case XgenScalarOp::logical_or: require_arity(2u); value = operand(0u) != 0.0 || operand(1u) != 0.0; break;
        case XgenScalarOp::select: require_arity(3u); value = operand(0u) != 0.0 ? operand(1u) : operand(2u); break;
        case XgenScalarOp::hash: {
            if (instruction.operand_count == 0u) {
                throw std::runtime_error("XGen scalar expression hash has no arguments");
            }
            std::vector<double> arguments;
            arguments.reserve(instruction.operand_count);
            for (std::size_t i = 0u; i < instruction.operand_count; ++i) {
                arguments.push_back(operand(i));
            }
            value = xgen_seexpr_hash(arguments);
            break;
        }
        case XgenScalarOp::random: {
            if (instruction.operand_count > 3u) {
                throw std::runtime_error("XGen scalar expression rand arity is invalid");
            }
            std::array<double, 5u> arguments{
                context.u, context.v, context.face_seed, instruction.immediate, 0.0};
            const bool explicit_seed = instruction.operand_count == 1u ||
                                       instruction.operand_count == 3u;
            if (explicit_seed) { arguments[4] = operand(instruction.operand_count - 1u); }
            value = xgen_seexpr_hash(std::span{arguments}.first(explicit_seed ? 5u : 4u));
            if (instruction.operand_count >= 2u) {
                value = (operand(1u) - operand(0u)) * value + operand(0u);
            }
            break;
        }
        case XgenScalarOp::minimum: require_arity(2u); value = std::min(operand(0u), operand(1u)); break;
        case XgenScalarOp::maximum: require_arity(2u); value = std::max(operand(0u), operand(1u)); break;
        case XgenScalarOp::clamp: require_arity(3u); value = std::clamp(operand(0u), operand(1u), operand(2u)); break;
        case XgenScalarOp::fit:
            require_arity(5u);
            value = operand(3u) + (operand(0u) - operand(1u)) *
                (operand(4u) - operand(3u)) / (operand(2u) - operand(1u));
            break;
        case XgenScalarOp::ramp: {
            require_arity(0u);
            if (instruction.auxiliary >= program.ramps.size()) {
                throw std::runtime_error("XGen expression ramp index is invalid");
            }
            const XgenRamp ramp = program.ramps[instruction.auxiliary];
            const std::size_t end = static_cast<std::size_t>(ramp.point_offset) +
                                    ramp.point_count;
            if (end > program.ramp_points.size()) {
                throw std::runtime_error("XGen expression ramp range is invalid");
            }
            value = evaluate_ramp_points<double>(
                std::span{program.ramp_points}.subspan(
                    ramp.point_offset, ramp.point_count),
                context.t);
            break;
        }
        }
        if (!std::isfinite(value)) {
            throw std::runtime_error("XGen scalar expression produced a non-finite value");
        }
        values.push_back(value);
    }
    if (program.result >= values.size()) {
        throw std::runtime_error("XGen scalar expression result index is invalid");
    }
    return values[program.result];
}

float evaluate_xgen_scalar_expression_float(
    const XgenFloatExpressionProgram &program,
    const XgenExpressionFloatContext &context) {
    if (context.inputs.size() != program.inputs.size()) {
        throw std::runtime_error("XGen float expression input count mismatch");
    }
    if (!std::isfinite(context.u) || !std::isfinite(context.v) ||
        !std::isfinite(context.face_seed) || !std::isfinite(context.t)) {
        throw std::runtime_error("XGen float expression context is non-finite");
    }
    for (const float input : context.inputs) {
        if (!std::isfinite(input)) {
            throw std::runtime_error("XGen float expression input is non-finite");
        }
    }
    std::vector<float> values;
    values.reserve(program.instructions.size());
    for (const XgenFloatScalarInstruction &instruction : program.instructions) {
        const std::size_t end = static_cast<std::size_t>(instruction.operand_offset) +
                                instruction.operand_count;
        if (end > program.operands.size()) {
            throw std::runtime_error("XGen float expression has an invalid operand range");
        }
        auto operand = [&](std::size_t index) -> float {
            if (index >= instruction.operand_count) {
                throw std::runtime_error("XGen float expression instruction arity mismatch");
            }
            const std::uint32_t value = program.operands[instruction.operand_offset + index];
            if (value >= values.size()) {
                throw std::runtime_error("XGen float expression violates SSA ordering");
            }
            return values[value];
        };
        auto require_arity = [&](std::size_t count) {
            if (instruction.operand_count != count) {
                throw std::runtime_error("XGen float expression instruction arity mismatch");
            }
        };
        float value{};
        switch (instruction.op) {
        case XgenScalarOp::constant:
            require_arity(0u); value = instruction.immediate; break;
        case XgenScalarOp::input:
            require_arity(0u);
            if (instruction.auxiliary >= context.inputs.size()) {
                throw std::runtime_error("XGen float expression input index is invalid");
            }
            value = context.inputs[instruction.auxiliary];
            break;
        case XgenScalarOp::negate: require_arity(1u); value = -operand(0u); break;
        case XgenScalarOp::logical_not: require_arity(1u); value = operand(0u) == 0.0f; break;
        case XgenScalarOp::add: require_arity(2u); value = operand(0u) + operand(1u); break;
        case XgenScalarOp::subtract: require_arity(2u); value = operand(0u) - operand(1u); break;
        case XgenScalarOp::multiply: require_arity(2u); value = operand(0u) * operand(1u); break;
        case XgenScalarOp::divide: require_arity(2u); value = operand(0u) / operand(1u); break;
        case XgenScalarOp::less: require_arity(2u); value = operand(0u) < operand(1u); break;
        case XgenScalarOp::less_equal: require_arity(2u); value = operand(0u) <= operand(1u); break;
        case XgenScalarOp::greater: require_arity(2u); value = operand(0u) > operand(1u); break;
        case XgenScalarOp::greater_equal: require_arity(2u); value = operand(0u) >= operand(1u); break;
        case XgenScalarOp::equal: require_arity(2u); value = operand(0u) == operand(1u); break;
        case XgenScalarOp::not_equal: require_arity(2u); value = operand(0u) != operand(1u); break;
        case XgenScalarOp::logical_and:
            require_arity(2u); value = operand(0u) != 0.0f && operand(1u) != 0.0f; break;
        case XgenScalarOp::logical_or:
            require_arity(2u); value = operand(0u) != 0.0f || operand(1u) != 0.0f; break;
        case XgenScalarOp::select:
            require_arity(3u); value = operand(0u) != 0.0f ? operand(1u) : operand(2u); break;
        case XgenScalarOp::hash: {
            std::vector<float> arguments;
            arguments.reserve(instruction.operand_count);
            for (std::size_t i = 0u; i < instruction.operand_count; ++i) {
                arguments.push_back(operand(i));
            }
            value = xgen_runtime_hash(arguments);
            break;
        }
        case XgenScalarOp::random: {
            std::array<float, 5u> arguments{context.u, context.v,
                context.face_seed, instruction.immediate, 0.0f};
            const bool explicit_seed = instruction.operand_count == 1u ||
                                       instruction.operand_count == 3u;
            if (explicit_seed) { arguments[4] = operand(instruction.operand_count - 1u); }
            value = xgen_runtime_hash(
                std::span{arguments}.first(explicit_seed ? 5u : 4u));
            if (instruction.operand_count >= 2u) {
                value = (operand(1u) - operand(0u)) * value + operand(0u);
            }
            break;
        }
        case XgenScalarOp::minimum: require_arity(2u); value = std::min(operand(0u), operand(1u)); break;
        case XgenScalarOp::maximum: require_arity(2u); value = std::max(operand(0u), operand(1u)); break;
        case XgenScalarOp::clamp: require_arity(3u); value = std::clamp(operand(0u), operand(1u), operand(2u)); break;
        case XgenScalarOp::fit:
            require_arity(5u);
            value = operand(3u) + (operand(0u) - operand(1u)) *
                (operand(4u) - operand(3u)) / (operand(2u) - operand(1u));
            break;
        case XgenScalarOp::ramp: {
            require_arity(0u);
            if (instruction.auxiliary >= program.ramps.size()) {
                throw std::runtime_error("XGen float expression ramp index is invalid");
            }
            const XgenRamp ramp = program.ramps[instruction.auxiliary];
            const std::size_t end = static_cast<std::size_t>(ramp.point_offset) +
                                    ramp.point_count;
            if (end > program.ramp_points.size()) {
                throw std::runtime_error("XGen float expression ramp range is invalid");
            }
            value = evaluate_ramp_points<float>(
                std::span{program.ramp_points}.subspan(
                    ramp.point_offset, ramp.point_count),
                context.t);
            break;
        }
        }
        if (!std::isfinite(value)) {
            throw std::runtime_error("XGen float expression produced a non-finite value");
        }
        values.push_back(value);
    }
    if (program.result >= values.size()) {
        throw std::runtime_error("XGen float expression result index is invalid");
    }
    return values[program.result];
}

} // namespace nanoxgen
