#include "type.h"
#include "qumir/parser/core/printer.h"

namespace NQumir {
namespace NAst {

std::ostream& operator<<(std::ostream& os, const TType& expr)
{
    NCore::PrintType(os, &expr);
    return os;
}

} // namespace NAst
} // namespace NQumir
