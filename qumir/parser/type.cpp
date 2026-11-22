#include "type.h"
#include "qumir/parser/core/printer.h"

#include <memory>

namespace NQumir {
namespace NAst {

std::ostream& operator<<(std::ostream& os, const TType& expr)
{
    // `expr` may not be owned by an intrusive pointer (e.g. a stack object):
    // hold an extra reference so the temporary pointer never deletes it.
    auto& mutableExpr = const_cast<TType&>(expr);
    mutableExpr.Ref();
    NCore::PrintType(os, TTypePtr(&mutableExpr));
    --mutableExpr.Count;
    return os;
}

} // namespace NAst
} // namespace NQumir
