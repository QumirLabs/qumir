#include "lexer_base.h"
#include <iostream>
namespace NQumir {
namespace NAst {

namespace {

static constexpr auto Eof = std::istream::traits_type::eof();

} // namespace

// Interpret a string literal value as a single character code if possible.
// Returns std::nullopt if the string does not represent exactly one character.
std::optional<int64_t> AsSingleCharCode(const std::string& s) {
    if (s.empty()) {
        return std::nullopt;
    }
    // Handle plain ASCII-single-byte case quickly
    if (static_cast<unsigned char>(s[0]) < 0x80) {
        if (s.size() != 1) {
            return std::nullopt;
        }
        return static_cast<int64_t>(static_cast<unsigned char>(s[0]));
    }

    // Validate exactly one well-formed UTF-8 codepoint (no overlong forms,
    // surrogates, values above U+10FFFF or trailing bytes).
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    size_t len = s.size();
    unsigned char lead = p[0];
    size_t need;
    int64_t code;
    int64_t min;
    if ((lead & 0b11100000) == 0b11000000) {
        need = 2;
        code = lead & 0b00011111;
        min = 0x80;
    } else if ((lead & 0b11110000) == 0b11100000) {
        need = 3;
        code = lead & 0b00001111;
        min = 0x800;
    } else if ((lead & 0b11111000) == 0b11110000) {
        need = 4;
        code = lead & 0b00000111;
        min = 0x10000;
    } else {
        return std::nullopt;
    }
    if (len != need) {
        return std::nullopt;
    }
    for (size_t i = 1; i < need; ++i) {
        if ((p[i] & 0b11000000) != 0b10000000) {
            return std::nullopt;
        }
        code = (code << 6) | (p[i] & 0b00111111);
    }
    if (code < min || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return std::nullopt;
    }
    return code;
}

char Unescape(char ch) {
    switch (ch) {
        case 'n': return '\n';
        case 't': return '\t';
        case '\\': return '\\';
        case '"': return '"';
        case '\'': return '\'';
        default:
            throw std::runtime_error("unknown escape sequence: \\" + std::string(1, ch));
    }
}

bool IsUtf8ContinuationByte(char ch) {
    return (static_cast<unsigned char>(ch) & 0b11000000) == 0b10000000;
}

void AdvanceLocation(TLocation& location, char ch) {
    if (ch == '\n') {
        location.Line++;
        location.Byte = 1;
        location.Column = 1;
        return;
    }

    if (!IsUtf8ContinuationByte(ch)) {
        location.Column++;
    }
    location.Byte++;
}

ITokenStream::ITokenStream(std::istream& in)
    : In(in)
    , CurrentLocation({1, 1, 1})
{ }

TToken ITokenStream::Next() {
    if (Tokens.empty()) {
        Read();
    }
    if (Tokens.empty()) {
        return TToken {
            .Value = {.i64 = -1},
            .Type = TToken::Operator,
            .Location = CurrentLocation
        };
    }
    TToken token = std::move(Tokens.front()); Tokens.pop_front();
    if (token.Type != TToken::Operator || token.Value.i64 != (uint8_t)-2) {
        SeenFirstToken = true;
    }
    return token;
}

char ITokenStream::Take() {
    char ch = 0;
    In.get(ch);
    AdvanceLocation(CurrentLocation, ch);
    return ch;
}

std::pair<std::string, std::string> ITokenStream::ReadQuoted(char quote) {
    std::string value;
    std::string rawValue;
    bool escaped = false;

    while (In.peek() != Eof) {
        char ch = Take();
        rawValue += ch;
        if (escaped) {
            value += Unescape(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == quote) {
            rawValue.pop_back();
            return std::make_pair(value, rawValue);
        } else {
            value += ch;
        }
    }

    throw std::runtime_error("unterminated literal");
}

void ITokenStream::ReadNumber(TLocation location) {
    std::string rawValue;
    bool isFloat = false;

    if (In.peek() == '-') {
        rawValue += Take();
    }

    if (In.peek() == '.') {
        isFloat = true;
        rawValue += Take();
    }

    while (In.peek() != Eof && std::isdigit(In.peek())) {
        rawValue += Take();
    }

    if (In.peek() == '.') {
        isFloat = true;
        rawValue += Take();
        while (In.peek() != Eof && std::isdigit(In.peek())) {
            rawValue += Take();
        }
    }

    if (In.peek() == 'e' || In.peek() == 'E') {
        isFloat = true;
        rawValue += Take();
        if (In.peek() == '+' || In.peek() == '-') {
            rawValue += Take();
        }
        if (In.peek() == Eof || !std::isdigit(In.peek())) {
            throw std::runtime_error("expected digit in exponent at " + CurrentLocation.ToString());
        }
        while (In.peek() != Eof && std::isdigit(In.peek())) {
            rawValue += Take();
        }
    }

    if (isFloat) {
        Tokens.emplace_back(TToken {
            .Value = {.f64 = std::stod(rawValue)},
            .RawValue = rawValue,
            .Type = TToken::Float,
            .Location = location,
        });
    } else {
        Tokens.emplace_back(TToken {
            .Value = {.i64 = std::stoll(rawValue)},
            .RawValue = rawValue,
            .Type = TToken::Integer,
            .Location = location,
        });
    }
}

void ITokenStream::Unget(TToken token) {
    Tokens.emplace_front(std::move(token));
}

const TLocation& ITokenStream::operator()() const {
    return CurrentLocation;
}

const TLocation& ITokenStream::GetLocation() const {
    return CurrentLocation;
}

TWrappedTokenStream::TWrappedTokenStream(ITokenStream& baseStream, int windowSize)
    : BaseStream(baseStream)
    , WindowSize(windowSize)
{ }

TToken TWrappedTokenStream::Next() {
    auto tok = BaseStream.Next();
    Window.push_back(tok);
    while (static_cast<int>(Window.size()) > WindowSize) {
        Window.pop_front();
    }
    return tok;
}

void TWrappedTokenStream::Unget(TToken token) {
    BaseStream.Unget(token);
    // A parser may unget more tokens than the window keeps.
    if (!Window.empty()) {
        Window.pop_back();
    }
}

const TLocation& TWrappedTokenStream::operator()() const {
    return BaseStream();
}

const TLocation& TWrappedTokenStream::GetLocation() const {
    return BaseStream.GetLocation();
}

} // namespace NAst
} // namespace NQumir
