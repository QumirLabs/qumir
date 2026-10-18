#include "pipeline.h"

namespace NQumir {
namespace NSemantics {
namespace NKumir {

std::expected<bool, TError> PowerTransform(
    NAst::TExprPtr& expr,
    TNameResolver&)
{
    bool changed = NAst::TransformAst(
        expr,
        expr,
        [](const NAst::TExprPtr& node) -> NAst::TExprPtr {
            auto binary = NAst::TMaybeNode<NAst::TBinaryExpr>(node);
            if (!binary || binary.Cast()->Operator != "**") {
                return node;
            }
            auto rightType = NAst::UnwrapReferenceType(binary.Cast()->Right->Type);
            std::string functionName = "pow";
            if (NAst::TMaybeType<NAst::TIntegerType>(rightType)) {
                functionName = "fpow";
            }
            return std::make_shared<NAst::TCallExpr>(
                binary.Cast()->Location,
                std::make_shared<NAst::TIdentExpr>(
                    binary.Cast()->Location,
                    std::move(functionName)),
                std::vector<NAst::TExprPtr>{
                    binary.Cast()->Left,
                    binary.Cast()->Right,
                });
        },
        [](const NAst::TExprPtr&) {
            return true;
        });
    return changed;
}

NTransform::TPipelineExtensions PipelineExtensions() {
    NTransform::TPipelineExtensions extensions;
    extensions.AfterTypeAnnotation.push_back(PowerTransform);
    extensions.AfterTypeAnnotation.push_back(NTransform::CoroutineAnnotationTransform);
    return extensions;
}

std::unordered_map<std::string, std::string> ModuleAliases() {
    return {
        {"Файлы", "System"},
        {"Строки", "System"},
    };
}

} // namespace NKumir
} // namespace NSemantics
} // namespace NQumir
