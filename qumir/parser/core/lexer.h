#pragma once

#include <qumir/location.h>
#include <qumir/parser/lexer_base.h>

#include <cstdint>
#include <deque>
#include <istream>
#include <string>
#include <string_view>

namespace NQumir {
namespace NAst {
namespace NCore {

class TTokenStream : public ITokenStream {
public:
    explicit TTokenStream(std::istream& in);

private:
    void Read() override;
    bool AfterOpenParen_ = false; // track previous token to disambiguate >> inside (>> a b)
};

// True for a letter, '_', '$' or non-ASCII byte followed by identifier characters:
// the lexer reads such a name as this one identifier wherever it stands.
bool IsPlainIdentifier(std::string_view name);

// True if `name` starts with a letter, '_', '$' or non-ASCII byte: the lexer reads it as an
// identifier up to the first character that cannot continue one.
bool StartsPlainIdentifier(std::string_view name);

} // namespace NCore
} // namespace NAst
} // namespace NQumir
