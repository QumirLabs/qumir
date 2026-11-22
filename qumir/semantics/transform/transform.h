#pragma once

#include <qumir/parser/ast.h>
#include <qumir/error.h>

#include <qumir/semantics/name_resolution/name_resolver.h>

#include <expected>
#include <functional>
#include <vector>

namespace NQumir {
namespace NTransform {

using TTransformPass = std::function<std::expected<bool, TError>(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context)>;

struct TPipelineExtensions {
    std::vector<TTransformPass> BeforeNameResolution;
    std::vector<TTransformPass> AfterNameResolution;
    std::vector<TTransformPass> AfterTypeAnnotation;
};

struct TPipelineOptions {
    TPipelineExtensions Extensions;
    bool RunDefiniteAssignment = true;
};

std::expected<bool, TError> PreNameResolutionTransform(NAst::TTypeFactory& types, NAst::TExprPtr& expr);
std::expected<bool, TError> PostNameResolutionTransform(NAst::TExprPtr& expr, NSemantics::TNameResolver& r);
std::expected<bool, TError> PostTypeAnnotationTransform(NAst::TExprPtr& expr, NSemantics::TNameResolver& r);
std::expected<bool, TError> CoroutineAnnotationTransform(NAst::TExprPtr& expr, NSemantics::TNameResolver& r);

std::expected<std::monostate, TError> RunSourceTransformFixpoint(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context,
    TPipelineOptions options = {});

std::expected<std::monostate, TError> FinalNameResolution(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context);

std::expected<std::monostate, TError> FinalTypeAnnotation(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context);

std::expected<std::monostate, TError> RunFinalSemanticPipeline(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context);

std::expected<std::monostate, TError> Pipeline(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context,
    TPipelineOptions options = {});

} // namespace NTransform
} // namespace NQumir
