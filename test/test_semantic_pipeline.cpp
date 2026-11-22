#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/modules/module.h>
#include <qumir/modules/system/system.h>
#include <qumir/modules/turtle/turtle.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/lexer.h>
#include <qumir/parser/parser.h>
#include <qumir/semantics/kumir/pipeline.h>
#include <qumir/semantics/lifetime/pass.h>
#include <qumir/semantics/lifetime/synthetic_name_generator.h>
#include <qumir/semantics/return_normalization/pass.h>
#include <qumir/semantics/transform/transform.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace NQumir;
using namespace NQumir::NAst;
using namespace NQumir::NRegistry;
using namespace NQumir::NSemantics;

namespace {

class TCollisionModule final : public IModule {
public:
    TCollisionModule()
        : Functions({TExternalFunction {
            .Name = "__lifetime_0",
            .MangledName = "collision_lifetime_0",
            .ReturnType = NQumir::NTest::TestTypes().Make<TVoidType>(),
        }})
        , Types({TExternalType {
            .Name = "__lifetime_1",
            .Type = NQumir::NTest::TestTypes().MakeInteger(),
        }})
    { }

    const std::string& Name() const override { return ModuleName; }
    const std::vector<TExternalFunction>& ExternalFunctions() const override { return Functions; }
    const std::vector<TExternalType>& ExternalTypes() const override { return Types; }
    const std::vector<TLiteralSuffix>& LiteralSuffixes() const override { return Suffixes; }
    const std::vector<std::string>& Dependencies() const override { return ModuleDependencies; }

private:
    std::string ModuleName = "SyntheticCollision";
    std::vector<TExternalFunction> Functions;
    std::vector<TExternalType> Types;
    std::vector<TLiteralSuffix> Suffixes;
    std::vector<std::string> ModuleDependencies;
};

TIntrusivePtr<TBlockExpr> MakeRoot(std::vector<TExprPtr> statements) {
    return MakeIntrusive<TBlockExpr>(TLocation{}, std::move(statements));
}

TIntrusivePtr<TFunDecl> MakeVoidFunction(
    std::string name,
    const TIntrusivePtr<TBlockExpr>& body)
{
    return MakeIntrusive<TFunDecl>(
        TLocation{},
        std::move(name),
        std::vector<TGenericParam>{},
        std::vector<TParam>{},
        body,
        NQumir::NTest::TestTypes().Make<TVoidType>());
}

std::string Print(const TExprPtr& expr) {
    NAst::NCore::TPrintOptions options;
    options.Pretty = false;
    return NAst::NCore::PrintAst(expr, options);
}

TExprPtr ParseCore(const std::string& source) {
    std::istringstream input(source);
    NAst::NCore::TTokenStream tokens(input);
    NAst::NCore::TParser parser(NQumir::NTest::TestTypes());
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        ADD_FAILURE() << parsed.error().ToString();
        return nullptr;
    }
    return std::move(*parsed);
}

TFieldAccessExpr* FindFieldAccess(const TExprPtr& expr, const std::string& fieldName) {
    if (!expr) {
        return nullptr;
    }
    if (auto field = TMaybeNode<TFieldAccessExpr>(expr).Cast(); field && field->FieldName == fieldName) {
        return field;
    }
    for (const auto& child : expr->Children()) {
        if (auto found = FindFieldAccess(child, fieldName)) {
            return found;
        }
    }
    return nullptr;
}

TCallExpr* FindCallWithCalleePrefix(const TExprPtr& expr, const std::string& prefix) {
    if (!expr) {
        return nullptr;
    }
    if (auto call = TMaybeNode<TCallExpr>(expr).Cast()) {
        if (auto callee = TMaybeNode<TIdentExpr>(call->Callee).Cast();
            callee && callee->Name.rfind(prefix, 0) == 0)
        {
            return call;
        }
    }
    for (const auto& child : expr->Children()) {
        if (auto found = FindCallWithCalleePrefix(child, prefix)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

TEST(SyntheticNameGenerator, AvoidsSourceAndImportedNamesPerPass) {
    TCollisionModule module;
    TNameResolver resolver(NQumir::NTest::TestTypes());
    resolver.RegisterModule(&module);
    auto importResult = resolver.ImportModule(module.Name());
    ASSERT_TRUE(importResult.has_value()) << importResult.error();

    auto sourceVar = MakeIntrusive<TVarStmt>(
        TLocation{},
        "__lifetime_2",
        NQumir::NTest::TestTypes().MakeInteger());
    TExprPtr root = MakeRoot({sourceVar});
    ASSERT_FALSE(resolver.Resolve(root).has_value());

    TSyntheticNameGenerator firstPass(resolver, root);
    EXPECT_EQ(firstPass.Next(), "__lifetime_3");
    EXPECT_EQ(firstPass.Next(), "__lifetime_4");

    TSyntheticNameGenerator secondPass(resolver, root);
    EXPECT_EQ(secondPass.Next(), "__lifetime_3");
}

TEST(FinalSemanticPipeline, InitialPassesDoNotChangeAst) {
    TExprPtr root = MakeRoot({});
    const auto original = root;
    TNameResolver resolver(NQumir::NTest::TestTypes());
    TSyntheticNameGenerator names(resolver, root);

    auto returnResult = ReturnNormalizationPass(root, resolver);
    ASSERT_TRUE(returnResult.has_value());
    EXPECT_FALSE(returnResult.value());

    auto lifetimeResult = LifetimePass(root, resolver, names);
    ASSERT_TRUE(lifetimeResult.has_value());
    EXPECT_FALSE(lifetimeResult.value());
    EXPECT_EQ(root, original);
}

TEST(SourceTransformExtensions, RunInStablePhaseOrder) {
    TExprPtr root = MakeRoot({});
    TNameResolver resolver(NQumir::NTest::TestTypes());
    std::vector<std::string> phases;
    NTransform::TPipelineOptions options;
    options.Extensions.BeforeNameResolution.push_back(
        [&](TExprPtr&, TNameResolver&) -> std::expected<bool, TError> {
            phases.push_back("before-name");
            return false;
        });
    options.Extensions.AfterNameResolution.push_back(
        [&](TExprPtr&, TNameResolver&) -> std::expected<bool, TError> {
            phases.push_back("after-name");
            return false;
        });
    options.Extensions.AfterTypeAnnotation.push_back(
        [&](TExprPtr&, TNameResolver&) -> std::expected<bool, TError> {
            phases.push_back("after-type");
            return false;
        });

    auto result = NTransform::RunSourceTransformFixpoint(
        root,
        resolver,
        std::move(options));

    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    EXPECT_EQ(
        phases,
        (std::vector<std::string>{
            "before-name",
            "after-name",
            "after-type",
            "before-name",
            "after-name",
        }));
}

TEST(SourceTransformExtensions, PropagateErrorsAndStopLaterPasses) {
    TExprPtr root = MakeRoot({});
    TNameResolver resolver(NQumir::NTest::TestTypes());
    bool laterPassRan = false;
    NTransform::TPipelineOptions options;
    options.Extensions.BeforeNameResolution.push_back(
        [](TExprPtr&, TNameResolver&) -> std::expected<bool, TError> {
            return std::unexpected(TError("injected failure"));
        });
    options.Extensions.AfterNameResolution.push_back(
        [&](TExprPtr&, TNameResolver&) -> std::expected<bool, TError> {
            laterPassRan = true;
            return false;
        });

    auto result = NTransform::RunSourceTransformFixpoint(
        root,
        resolver,
        std::move(options));

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().ToString().find("injected failure"), std::string::npos);
    EXPECT_FALSE(laterPassRan);
}

TEST(PostNameResolutionTransform, WrapsOnlyBareZeroArgumentFunctionIdentifiers) {
    auto callee = MakeVoidFunction("foo", MakeRoot({}));
    auto bareIdent = MakeIntrusive<TIdentExpr>(TLocation{}, "foo");
    auto explicitIdent = MakeIntrusive<TIdentExpr>(TLocation{}, "foo");
    auto explicitCall = MakeIntrusive<TCallExpr>(
        TLocation{}, explicitIdent, std::vector<TExprPtr>{});
    auto callerBody = MakeRoot({bareIdent, explicitCall});
    auto caller = MakeVoidFunction("caller", callerBody);
    TExprPtr root = MakeRoot({callee, caller});

    TNameResolver resolver(NQumir::NTest::TestTypes());
    ASSERT_FALSE(resolver.Resolve(root).has_value());

    auto result = NTransform::PostNameResolutionTransform(root, resolver);
    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    EXPECT_TRUE(*result);

    auto wrappedBare = TMaybeNode<TCallExpr>(callerBody->Stmts[0]).Cast();
    ASSERT_NE(wrappedBare, nullptr);
    EXPECT_EQ(wrappedBare->Callee, bareIdent);
    EXPECT_EQ(callerBody->Stmts[1], explicitCall);
    EXPECT_EQ(explicitCall->Callee, explicitIdent);
}

TEST(KumirPipeline, InjectsCoroutineAnnotationAfterTypeAnnotation) {
    auto extensions = NSemantics::NKumir::PipelineExtensions();

    EXPECT_TRUE(extensions.BeforeNameResolution.empty());
    EXPECT_TRUE(extensions.AfterNameResolution.empty());
    EXPECT_EQ(extensions.AfterTypeAnnotation.size(), 2u);
}

TEST(KumirPipeline, ParserEmitsDistinctPowerOperator) {
    NRegistry::SystemModule system(NQumir::NTest::TestTypes());
    TNameResolver resolver(NQumir::NTest::TestTypes());
    resolver.RegisterModule(&system);
    ASSERT_TRUE(resolver.ImportModule(system.Name()).has_value());
    std::istringstream input(
        "алг\n"
        "нач\n"
        "  цел x\n"
        "  x := 2 ** 3\n"
        "кон\n");
    TTokenStream tokens(input);
    TParser parser(NQumir::NTest::TestTypes());
    auto parsed = parser.parse(tokens, &resolver);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();
    auto root = TMaybeNode<TBlockExpr>(*parsed).Cast();
    ASSERT_NE(root, nullptr);
    auto function = TMaybeNode<TFunDecl>(root->Stmts.front()).Cast();
    ASSERT_NE(function, nullptr);
    auto assignment = TMaybeNode<TAssignExpr>(function->Body->Stmts[1]).Cast();
    ASSERT_NE(assignment, nullptr);
    auto power = TMaybeNode<TBinaryExpr>(assignment->Value).Cast();
    ASSERT_NE(power, nullptr);
    EXPECT_EQ(power->Operator.ToString(), "**");
}

TEST(KumirParser, ParameterAccessModesAreExplicit) {
    NRegistry::SystemModule system(NQumir::NTest::TestTypes());
    TNameResolver resolver(NQumir::NTest::TestTypes());
    resolver.RegisterModule(&system);
    ASSERT_TRUE(resolver.ImportModule(system.Name()).has_value());
    std::istringstream input(
        "алг test(арг цел a, рез цел b, аргрез цел c)\n"
        "нач\n"
        "кон\n");
    TTokenStream tokens(input);
    TParser parser(NQumir::NTest::TestTypes());

    auto parsed = parser.parse(tokens, &resolver);

    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();
    auto root = TMaybeNode<TBlockExpr>(*parsed).Cast();
    ASSERT_NE(root, nullptr);
    auto function = TMaybeNode<TFunDecl>(root->Stmts.front()).Cast();
    ASSERT_NE(function, nullptr);
    ASSERT_EQ(function->Params.size(), 3u);
    const auto& inputAccess = function->Params[0]->Access;
    const auto& outputAccess = function->Params[1]->Access;
    const auto& inOutAccess = function->Params[2]->Access;
    EXPECT_TRUE(inputAccess.Readable);
    EXPECT_FALSE(inputAccess.Mutable);
    EXPECT_FALSE(outputAccess.Readable);
    EXPECT_TRUE(outputAccess.Mutable);
    EXPECT_TRUE(inOutAccess.Readable);
    EXPECT_TRUE(inOutAccess.Mutable);
}

TEST(ModuleAliases, ResolveImportAndListThroughAlias) {
    NRegistry::SystemModule system(NQumir::NTest::TestTypes());
    TNameResolver resolver(NQumir::NTest::TestTypes());
    resolver.RegisterModule(&system);

    // Without an alias the legacy Kumir name is unknown.
    EXPECT_FALSE(resolver.ImportModule("Файлы").has_value());

    resolver.RegisterModuleAlias("Файлы", system.Name());
    EXPECT_TRUE(resolver.ImportModule("Файлы").has_value());
    EXPECT_NE(resolver.ModulesList().find("Файлы"), std::string::npos);
}

TEST(ModuleAliases, RejectsAliasToUnregisteredModule) {
    TNameResolver resolver(NQumir::NTest::TestTypes());
    EXPECT_THROW(resolver.RegisterModuleAlias("Файлы", "System"), std::runtime_error);
}

TEST(ModuleAliases, KumirFrontendAliasesAreSystem) {
    auto aliases = NKumir::ModuleAliases();
    EXPECT_EQ(aliases.at("Файлы"), "System");
    EXPECT_EQ(aliases.at("Строки"), "System");
}

// The same legacy name resolves only when the frontend registers the alias;
// core and Kumir sessions differ purely by configuration.
TEST(ModuleAliases, SameUseDiffersBetweenCoreAndKumirSessions) {
    NRegistry::SystemModule system(NQumir::NTest::TestTypes());

    TNameResolver core(NQumir::NTest::TestTypes());
    core.RegisterModule(&system);
    EXPECT_FALSE(core.ImportModule("Файлы").has_value());

    TNameResolver kumir(NQumir::NTest::TestTypes());
    kumir.RegisterModule(&system);
    for (const auto& [alias, canonical] : NKumir::ModuleAliases()) {
        kumir.RegisterModuleAlias(alias, canonical);
    }
    EXPECT_TRUE(kumir.ImportModule("Файлы").has_value());
}

TEST(ModulesList, ListsOnlyModulesProgramsImport) {
    NRegistry::SystemModule system(NQumir::NTest::TestTypes());
    NRegistry::TurtleModule turtle(NQumir::NTest::TestTypes());
    TNameResolver resolver(NQumir::NTest::TestTypes());
    resolver.RegisterModule(&system);
    resolver.RegisterModule(&turtle);
    EXPECT_EQ(resolver.ModulesList(), "Черепаха");
}

TEST(KumirPipeline, ExpandsPowerWithoutReusingCoreOperator) {
    NRegistry::SystemModule system(NQumir::NTest::TestTypes());
    TNameResolver resolver(NQumir::NTest::TestTypes());
    resolver.RegisterModule(&system);
    ASSERT_TRUE(resolver.ImportModule(system.Name()).has_value());
    auto right = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{3}, NQumir::NTest::TestTypes());
    right->Type = NQumir::NTest::TestTypes().MakeInteger();
    TExprPtr ast = MakeIntrusive<TBinaryExpr>(
        TLocation{},
        TOperator("**"),
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{2}, NQumir::NTest::TestTypes()),
        std::move(right));

    auto result = NSemantics::NKumir::PowerTransform(ast, resolver);

    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    EXPECT_TRUE(*result);
    auto call = TMaybeNode<TCallExpr>(ast).Cast();
    ASSERT_NE(call, nullptr);
    auto callee = TMaybeNode<TIdentExpr>(call->Callee).Cast();
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(callee->Name, "fpow");
    EXPECT_EQ(call->Args.size(), 2u);
}

TEST(KumirPipeline, ExpandsFloatPowerToPow) {
    NRegistry::SystemModule system(NQumir::NTest::TestTypes());
    TNameResolver resolver(NQumir::NTest::TestTypes());
    resolver.RegisterModule(&system);
    ASSERT_TRUE(resolver.ImportModule(system.Name()).has_value());
    TExprPtr ast = MakeIntrusive<TBinaryExpr>(
        TLocation{},
        TOperator("**"),
        MakeIntrusive<TNumberExpr>(TLocation{}, 2.0, NQumir::NTest::TestTypes()),
        MakeIntrusive<TNumberExpr>(TLocation{}, 0.5, NQumir::NTest::TestTypes()));

    auto result = NTransform::RunSourceTransformFixpoint(
        ast,
        resolver,
        {.Extensions = NSemantics::NKumir::PipelineExtensions()});

    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    auto call = TMaybeNode<TCallExpr>(ast).Cast();
    ASSERT_NE(call, nullptr);
    auto callee = TMaybeNode<TIdentExpr>(call->Callee).Cast();
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(callee->Name, "pow");
}

TEST(ReturnNormalization, MakesFallthroughReturnsExplicit) {
    auto implicitValue = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{42}, NQumir::NTest::TestTypes());
    auto valueBody = MakeRoot({implicitValue});
    auto valueFunction = MakeIntrusive<TFunDecl>(
        TLocation{},
        "value",
        std::vector<TGenericParam>{},
        std::vector<TParam>{},
        valueBody,
        NQumir::NTest::TestTypes().MakeInteger());
    auto voidBody = MakeRoot({});
    auto voidFunction = MakeVoidFunction("action", voidBody);
    TExprPtr root = MakeRoot({valueFunction, voidFunction});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = ReturnNormalizationPass(root, resolver);
    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    EXPECT_TRUE(result.value());
    EXPECT_EQ(Print(valueBody), "(block (return 42))");
    EXPECT_EQ(Print(voidBody), "(block (return))");
}

TEST(ReturnNormalization, PreservesDirectAndFullyTerminatingBranchReturns) {
    auto directReturn = MakeIntrusive<TReturnExpr>(
        TLocation{},
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes()));
    auto directBody = MakeRoot({directReturn});
    auto directFunction = MakeIntrusive<TFunDecl>(
        TLocation{},
        "direct",
        std::vector<TGenericParam>{},
        std::vector<TParam>{},
        directBody,
        NQumir::NTest::TestTypes().MakeInteger());
    auto assertCondition = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes());
    assertCondition->Type = NQumir::NTest::TestTypes().Make<TBoolType>();
    directFunction->LastAssert = MakeIntrusive<TAssertStmt>(
        TLocation{},
        assertCondition);

    auto condition = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes());
    condition->Type = NQumir::NTest::TestTypes().Make<TBoolType>();
    auto elseIfCondition = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{0}, NQumir::NTest::TestTypes());
    elseIfCondition->Type = NQumir::NTest::TestTypes().Make<TBoolType>();
    auto elseIf = MakeIntrusive<TIfExpr>(
        TLocation{},
        elseIfCondition,
        MakeRoot({MakeIntrusive<TReturnExpr>(
            TLocation{},
            MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{3}, NQumir::NTest::TestTypes()))}),
        MakeRoot({MakeIntrusive<TReturnExpr>(
            TLocation{},
            MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{4}, NQumir::NTest::TestTypes()))}));
    auto branch = MakeIntrusive<TIfExpr>(
        TLocation{},
        condition,
        MakeRoot({MakeIntrusive<TReturnExpr>(
            TLocation{},
            MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{2}, NQumir::NTest::TestTypes()))}),
        elseIf);
    auto branchBody = MakeRoot({MakeRoot({MakeRoot({branch})})});
    auto branchFunction = MakeIntrusive<TFunDecl>(
        TLocation{},
        "branch",
        std::vector<TGenericParam>{},
        std::vector<TParam>{},
        branchBody,
        NQumir::NTest::TestTypes().MakeInteger());
    TExprPtr root = MakeRoot({directFunction, branchFunction});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = ReturnNormalizationPass(root, resolver);
    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    EXPECT_FALSE(result.value());
    EXPECT_EQ(directBody->Stmts.size(), 1u);
    EXPECT_EQ(branchBody->Stmts.size(), 1u);
    EXPECT_EQ(
        Print(directFunction),
        "(fun direct () -> i64 (attrs (expect_after (assert #t))) "
        "(block (return 1)))");
    EXPECT_EQ(
        Print(branchBody),
        "(block (block (block (if #t (block (return 2)) "
        "(if #f (block (return 3)) (block (return 4)))))))");
}

TEST(FinalSemanticPipeline, ResolvesAndAnnotatesInsertedNestedBlock) {
    auto body = MakeRoot({});
    auto function = MakeVoidFunction("main", body);
    TExprPtr root = MakeRoot({function});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto sourceResult = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(sourceResult.has_value()) << sourceResult.error().ToString();

    TSyntheticNameGenerator names(resolver, root);
    const auto syntheticName = names.Next();
    auto syntheticVar = MakeIntrusive<TVarStmt>(
        TLocation{},
        syntheticName,
        NQumir::NTest::TestTypes().MakeInteger());
    syntheticVar->Init = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{7}, NQumir::NTest::TestTypes());
    auto syntheticUse = MakeIntrusive<TIdentExpr>(TLocation{}, syntheticName);
    auto nested = MakeRoot({syntheticVar, syntheticUse});
    body->Stmts.push_back(nested);
    body->Stmts.push_back(MakeIntrusive<TReturnExpr>(TLocation{}, nullptr));

    ASSERT_EQ(nested->Scope, -1);
    auto nameResult = NTransform::FinalNameResolution(root, resolver);
    ASSERT_TRUE(nameResult.has_value()) << nameResult.error().ToString();
    ASSERT_GE(nested->Scope, 0);
    EXPECT_NE(nested->Scope, body->Scope);

    auto symbol = resolver.Lookup(syntheticName, TScopeId{nested->Scope});
    ASSERT_TRUE(symbol.has_value());
    EXPECT_EQ(symbol->DeclScopeId, nested->Scope);
    EXPECT_EQ(symbol->FuncScopeId, function->Scope);
    EXPECT_EQ(resolver.GetSymbolNode(TSymbolId{symbol->Id}), syntheticVar.Get());

    auto typeResult = NTransform::FinalTypeAnnotation(root, resolver);
    ASSERT_TRUE(typeResult.has_value()) << typeResult.error().ToString();
    EXPECT_TRUE(TMaybeType<TIntegerType>(syntheticUse->Type));
}

TEST(ParametricTypes, ConcreteNamedTypeInstantiationResolvesUnderlyingFields) {
    auto root = ParseCore(R"__(
(block
  (type Nullable [T] <struct (Value T) (Valid i8)>)
  (fun unwrap ((var x <named Nullable [i64]>)) -> i64
    (block
      (return (field x Value)))))
)__");
    ASSERT_NE(root, nullptr);

    TNameResolver resolver(NQumir::NTest::TestTypes());
    auto nameResult = NTransform::FinalNameResolution(root, resolver);
    ASSERT_TRUE(nameResult.has_value()) << nameResult.error().ToString();
    auto typeResult = NTransform::FinalTypeAnnotation(root, resolver);
    ASSERT_TRUE(typeResult.has_value()) << typeResult.error().ToString();

    const auto& functionNode = TMaybeNode<TBlockExpr>(root).Cast()->Stmts[1];
    auto function = TMaybeNode<TFunDecl>(functionNode).Cast();
    ASSERT_TRUE(function);
    auto paramType = TMaybeType<TNamedType>(function->Params[0]->Type).Cast();
    ASSERT_TRUE(paramType);
    auto paramStruct = TMaybeType<TStructType>(paramType->UnderlyingType()).Cast();
    ASSERT_TRUE(paramStruct);
    ASSERT_EQ(paramStruct->Fields.size(), 2u);
    EXPECT_EQ(paramStruct->Fields[0].first, "Value");
    EXPECT_TRUE(TMaybeType<TIntegerType>(paramStruct->Fields[0].second));

    auto field = FindFieldAccess(functionNode, "Value");
    ASSERT_TRUE(field);
    EXPECT_TRUE(TMaybeType<TIntegerType>(field->Type));
}

TEST(ParametricTypes, GenericFunctionInstantiatesParametricNamedType) {
    auto root = ParseCore(R"__(
(block
  (type Box [T] <struct (Value T)>)
  (fun unwrap ((var text string)) -> i64
    (block
      (return (: 0 i64))))
  (fun unwrap [T] ((var box <named Box [T]>)) -> T
    (block
      (return (field box Value))))
  (fun run () -> i64
    (block
      (var boxed <named Box [i64]>)
      (= boxed (cast (struct ((Value (: 42 i64)))) <named Box [i64]>))
      (return (call unwrap boxed)))))
)__");
    ASSERT_NE(root, nullptr);

    TNameResolver resolver(NQumir::NTest::TestTypes(), {.AllowOverloads = true});
    auto nameResult = NTransform::FinalNameResolution(root, resolver);
    ASSERT_TRUE(nameResult.has_value()) << nameResult.error().ToString();
    auto typeResult = NTransform::FinalTypeAnnotation(root, resolver);
    ASSERT_TRUE(typeResult.has_value()) << typeResult.error().ToString();

    auto instantiatedCall = FindCallWithCalleePrefix(root, "__generic_");
    ASSERT_TRUE(instantiatedCall);
    auto callType = TMaybeType<TIntegerType>(instantiatedCall->Type).Cast();
    ASSERT_TRUE(callType);
    EXPECT_EQ(callType->Kind, TIntegerType::I64);
}

TEST(ParametricTypes, RejectsParameterizedGenericPlaceholder) {
    auto root = ParseCore(R"__(
(block
  (fun bad [T] ((var x T)) -> <named T [i64]>
    (block
      (return x))))
)__");
    ASSERT_NE(root, nullptr);

    TNameResolver resolver(NQumir::NTest::TestTypes());
    auto nameResult = NTransform::FinalNameResolution(root, resolver);
    ASSERT_FALSE(nameResult.has_value());
    EXPECT_NE(nameResult.error().ToString().find("Generic type parameter 'T' does not accept parameters"), std::string::npos);
}

TEST(ParametricTypes, RejectsAlphaEquivalentGenericOverloads) {
    auto root = ParseCore(R"__(
(block
  (fun id [T] ((var x T)) -> T
    (block
      (return x)))
  (fun id [U] ((var x U)) -> U
    (block
      (return x))))
)__");
    ASSERT_NE(root, nullptr);

    TNameResolver resolver(NQumir::NTest::TestTypes(), {.AllowOverloads = true});
    auto nameResult = NTransform::FinalNameResolution(root, resolver);
    ASSERT_FALSE(nameResult.has_value());
    EXPECT_NE(nameResult.error().ToString().find("overload of 'id' has identical parameter types"), std::string::npos);
}

TEST(ParametricTypes, RejectsAlphaEquivalentGenericOperatorOverloads) {
    auto root = ParseCore(R"__(
(block
  (type Box [T] <struct (Value T)>)
  (fun plus_a [T] ((var a <named Box [T]>) (var b <named Box [T]>)) -> <named Box [T]> (attrs (operator "+"))
    (block
      (return a)))
  (fun plus_b [U] ((var a <named Box [U]>) (var b <named Box [U]>)) -> <named Box [U]> (attrs (operator "+"))
    (block
      (return a))))
)__");
    ASSERT_NE(root, nullptr);

    TNameResolver resolver(NQumir::NTest::TestTypes(), {.AllowOverloads = true});
    auto nameResult = NTransform::FinalNameResolution(root, resolver);
    ASSERT_FALSE(nameResult.has_value());
    EXPECT_NE(nameResult.error().ToString().find("operator overload '+' has identical parameter types"), std::string::npos);
}

TEST(ParametricTypes, PrefersParametricGenericOverBareGenericOverload) {
    auto root = ParseCore(R"__(
(block
  (type Box [T] <struct (Value T)>)
  (fun f [T] ((var x T)) -> string
    (block
      (return "fallback")))
  (fun f [T] ((var x <named Box [T]>)) -> i64
    (block
      (return (: 2 i64))))
  (fun run () -> i64
    (block
      (var boxed <named Box [i64]>)
      (= boxed (cast (struct ((Value (: 42 i64)))) <named Box [i64]>))
      (return (call f boxed)))))
)__");
    ASSERT_NE(root, nullptr);

    TNameResolver resolver(NQumir::NTest::TestTypes(), {.AllowOverloads = true});
    auto nameResult = NTransform::FinalNameResolution(root, resolver);
    ASSERT_TRUE(nameResult.has_value()) << nameResult.error().ToString();
    auto typeResult = NTransform::FinalTypeAnnotation(root, resolver);
    ASSERT_TRUE(typeResult.has_value()) << typeResult.error().ToString();

    auto instantiatedCall = FindCallWithCalleePrefix(root, "__generic_");
    ASSERT_TRUE(instantiatedCall);
    auto callType = TMaybeType<TIntegerType>(instantiatedCall->Type).Cast();
    ASSERT_TRUE(callType);
    EXPECT_EQ(callType->Kind, TIntegerType::I64);
}

TEST(ParametricTypes, RetypesIntegerLiteralToExistingFloatGenericBinding) {
    auto root = ParseCore(R"__(
(block
  (pragma language overloads)
  (type Nullable [T] <struct (Value T) (Valid bool)>)
  (fun nullable_from_value [T] ((var value T)) -> <named Nullable [T]> (attrs (operator "cast"))
    (block
      (return (cast (struct ((Value value) (Valid #t))) <named Nullable [T]>))))
  (fun nullable_add [T] ((var a <named Nullable [T]>) (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "+"))
    (block
      (return a)))
  (fun nullable_add_rhs [T] ((var a <named Nullable [T]>) (var b T)) -> <named Nullable [T]> (attrs (operator "+"))
    (block
      (return (+ a (cast b <named Nullable [T]>)))))
  (fun run ((var a <named Nullable [f64]>)) -> <named Nullable [f64]>
    (block
      (return (+ a (: 1 i64))))))
)__");
    ASSERT_NE(root, nullptr);

    TNameResolver resolver(NQumir::NTest::TestTypes(), {.AllowOverloads = true});
    auto nameResult = NTransform::FinalNameResolution(root, resolver);
    ASSERT_TRUE(nameResult.has_value()) << nameResult.error().ToString();
    auto typeResult = NTransform::FinalTypeAnnotation(root, resolver);
    ASSERT_TRUE(typeResult.has_value()) << typeResult.error().ToString();

    auto instantiatedCall = FindCallWithCalleePrefix(root, "__generic_nullable_add_rhs$Nullable_f64__$Nullable_f64$f64");
    ASSERT_TRUE(instantiatedCall);
    auto nullable = TMaybeType<TNamedType>(instantiatedCall->Type).Cast();
    ASSERT_TRUE(nullable);
    ASSERT_EQ(nullable->TypeArgs.size(), 1u);
    EXPECT_TRUE(TMaybeType<TFloatType>(nullable->TypeArgs[0].Type));
}

TEST(FinalSemanticPipeline, DoesNotRerunSourceTransforms) {
    auto condition = MakeIntrusive<TNumberExpr>(TLocation{}, true, NQumir::NTest::TestTypes());
    auto assertNode = MakeIntrusive<TAssertStmt>(TLocation{}, condition);
    assertNode->Type = NQumir::NTest::TestTypes().Make<TVoidType>();
    auto body = MakeRoot({assertNode, MakeIntrusive<TReturnExpr>(TLocation{}, nullptr)});
    TExprPtr root = MakeRoot({MakeVoidFunction("main", body)});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    ASSERT_EQ(body->Stmts.size(), 2u);
    EXPECT_TRUE(TMaybeNode<TAssertStmt>(body->Stmts[0]));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
