#pragma once

#include <qumir/semantics/transform/transform.h>

#include <string>
#include <unordered_map>

namespace NQumir {
namespace NSemantics {
namespace NKumir {

std::expected<bool, TError> PowerTransform(
    NAst::TExprPtr& expr,
    TNameResolver& context);

NTransform::TPipelineExtensions PipelineExtensions();

// Kumir frontend module aliases: legacy standard-library names that resolve to
// the modules actually providing their symbols. Maps alias -> canonical name.
std::unordered_map<std::string, std::string> ModuleAliases();

} // namespace NKumir
} // namespace NSemantics
} // namespace NQumir
