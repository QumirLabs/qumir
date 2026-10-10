#pragma once

#include <string_view>

namespace NQumir {
namespace NSemantics {

inline std::string_view VectorIntrinsic(std::string_view name) {
    if (name.starts_with("__generic_builtin::")) {
        name.remove_prefix(10);
        name = name.substr(0, name.find('$'));
    }
    if (!name.starts_with("builtin::")) {
        return {};
    }
    name.remove_prefix(9);
    if (name == "all" || name == "any" || name == "shuffle") {
        return name;
    }
    return {};
}

} // namespace NSemantics
} // namespace NQumir
