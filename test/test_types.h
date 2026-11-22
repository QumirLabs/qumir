#pragma once

#include <qumir/parser/type_factory.h>

namespace NQumir {
namespace NTest {

// One type factory per test binary: all modules, parsers and passes of a test share it,
// so equal types are the same object. Types are immutable, so tests cannot interfere.
inline NAst::TTypeFactory& TestTypes() {
    static NAst::TTypeFactory types;
    return types;
}

} // namespace NTest
} // namespace NQumir
