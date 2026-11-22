#pragma once

#include <expected>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>
#include <qumir/parser/parser.h>

#include <qumir/semantics/name_resolution/name_resolver.h>

namespace NQumir {
namespace NTypeAnnotation {

class TTypeAnnotator {
public:
    explicit TTypeAnnotator(NSemantics::TNameResolver& context);

    std::expected<NAst::TExprPtr, TError> Annotate(const NAst::TExprPtr& expr);

private:
    NSemantics::TNameResolver& Context;
};

} // namespace NTypeAnnotation
} // namespace NQumir
