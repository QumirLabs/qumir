#pragma once

#include <ostream>
#include <string>

namespace NQumir {
namespace NCodeGen {

// Links a host object with libqumir_runtime.a into an executable written to
// `os`. A failed link prints the linker output to stderr and writes nothing.
void LinkExecutable(const std::string& object, std::ostream& os);

} // namespace NCodeGen
} // namespace NQumir
