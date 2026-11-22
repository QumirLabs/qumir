#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/location.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <array>
#include <sstream>

using namespace NQumir;
using namespace NQumir::NAst;
using namespace NQumir::NAst::NCore;

namespace {

// Custom node: (tag "label" <child>)
struct TTagExpr : TExpr {
    static constexpr const char* NodeId = "Tag";

    std::string Label;
    TExprPtr Child;

    TTagExpr(std::string label, TExprPtr child)
        : Label(std::move(label))
        , Child(std::move(child))
    {}

    const std::string_view NodeName() const override { return NodeId; }

    std::vector<TExprPtr> Children() const override { return {Child}; }

    void Accept(IVisitor& visitor) override { visitor.VisitOtherwise(*this); }
};

// Custom node: (pair <left> <right>)
struct TPairExpr : TExpr {
    static constexpr const char* NodeId = "Pair";

    TExprPtr Left;
    TExprPtr Right;

    TPairExpr(TExprPtr left, TExprPtr right)
        : Left(std::move(left))
        , Right(std::move(right))
    {}

    const std::string_view NodeName() const override { return NodeId; }

    std::vector<TExprPtr> Children() const override { return {Left, Right}; }

    void Accept(IVisitor& visitor) override { visitor.VisitOtherwise(*this); }
};

TPrintOptions MakeOptions(bool pretty = false) {
    TPrintOptions opts;
    opts.Pretty = pretty;
    opts.NodePrinters[TTagExpr::NodeId] = [](TExpr& node, TPrinter& p, TPrintFrame frame) {
        auto& tag = static_cast<TTagExpr&>(node);
        p.GetOut() << "(tag \"" << tag.Label << "\"";
        p.Separator(frame.Level + 1);
        p.PrintExpr(tag.Child, frame.AllowTypeWrap, frame.Level + 1);
        p.GetOut() << ')';
    };
    opts.NodePrinters[TPairExpr::NodeId] = [](TExpr& node, TPrinter& p, TPrintFrame frame) {
        auto& pair = static_cast<TPairExpr&>(node);
        p.GetOut() << "(pair";
        p.Separator(frame.Level + 1);
        p.PrintExpr(pair.Left, frame.AllowTypeWrap, frame.Level + 1);
        p.Separator(frame.Level + 1);
        p.PrintExpr(pair.Right, frame.AllowTypeWrap, frame.Level + 1);
        p.GetOut() << ')';
    };
    return opts;
}

TExprPtr MakeIdent(std::string name) {
    return MakeIntrusive<TIdentExpr>(TLocation{}, std::move(name));
}

} // namespace

namespace {
template<typename To>
const To* DynCast(TTypePtr from) {
    return dynamic_cast<const To*>(from);
}
} // namespace

TEST(PrinterCustomNodes, TagCompact) {
    auto expr = MakeIntrusive<TTagExpr>("hello", MakeIdent("x"));
    EXPECT_EQ(PrintAst(expr, MakeOptions()), "(tag \"hello\" x)");
}

namespace {

// Counts destructions: a node printer must not lose its captured state while it runs.
struct TStateGuard {
    static inline int Destroyed = 0;
    static inline int DestroyedWhileChildPrinted = -1;
    std::array<char, 64> Pad{};

    TStateGuard() = default;
    TStateGuard(const TStateGuard&) = default;
    ~TStateGuard()
    {
        ++Destroyed;
    }
};

} // namespace

TEST(PrinterCustomNodes, NodePrinterKeepsItsStateWhilePrintingAChild) {
    TPrintOptions opts;
    opts.LineWidth = 20;
    opts.NodePrinters[TTagExpr::NodeId] =
        [guard = TStateGuard()](TExpr& node, TPrinter& p, TPrintFrame frame) {
            auto& tag = static_cast<TTagExpr&>(node);
            p.GetOut() << "(tag \"" << tag.Label << "\"";
            p.Separator(frame.Level + 1);
            int before = TStateGuard::Destroyed;
            // The child fits one line, so the printer renders it compactly.
            p.PrintExpr(tag.Child, frame.AllowTypeWrap, frame.Level + 1);
            TStateGuard::DestroyedWhileChildPrinted = TStateGuard::Destroyed - before;
            p.GetOut() << ')';
        };
    auto expr = MakeIntrusive<TTagExpr>("a label longer than the line", MakeIdent("x"));
    EXPECT_EQ(PrintAst(expr, opts), "(tag \"a label longer than the line\"\n  x)");
    EXPECT_EQ(TStateGuard::DestroyedWhileChildPrinted, 0);
}

TEST(PrinterNumbers, IntegralFloatKeepsItsLiteralType) {
    EXPECT_EQ(PrintAst(MakeIntrusive<TNumberExpr>(TLocation{}, 1.0, NQumir::NTest::TestTypes())), "1.0");
    EXPECT_EQ(PrintAst(MakeIntrusive<TNumberExpr>(TLocation{}, -0.0, NQumir::NTest::TestTypes())), "-0.0");
    EXPECT_EQ(PrintAst(MakeIntrusive<TNumberExpr>(TLocation{}, 1e20, NQumir::NTest::TestTypes())), "1e+20");
}

TEST(PrinterCustomNodes, PairCompact) {
    auto expr = MakeIntrusive<TPairExpr>(MakeIdent("a"), MakeIdent("b"));
    EXPECT_EQ(PrintAst(expr, MakeOptions()), "(pair a b)");
}

TEST(PrinterCustomNodes, NestedCustomNodes) {
    auto inner = MakeIntrusive<TPairExpr>(MakeIdent("x"), MakeIdent("y"));
    auto outer = MakeIntrusive<TTagExpr>("wrap", inner);
    EXPECT_EQ(PrintAst(outer, MakeOptions()), "(tag \"wrap\" (pair x y))");
}

TEST(PrinterCustomNodes, UnknownNodeThrows) {
    struct TUnknown : TExpr {
        const std::string_view NodeName() const override { return "Unknown"; }
        void Accept(IVisitor& v) override { v.VisitOtherwise(*this); }
    };
    auto expr = MakeIntrusive<TUnknown>();
    TPrintOptions opts;
    EXPECT_THROW(PrintAst(expr, opts), std::runtime_error);
}

namespace {

TParser MakeParser() {
    TParser p(NQumir::NTest::TestTypes());
    p.NodeParsers["tag"] = [](IParseHandle& h, TLocation loc) -> TAstTask {
        auto tok = h.Next();
        if (tok.Type != TToken::String) {
            co_return IParseHandle::MakeError(tok, "expected string label for tag");
        }
        auto child = co_await h.Expr();
        co_await h.Take(')');
        co_return MakeIntrusive<TTagExpr>(tok.Name, std::move(child));
    };
    p.NodeParsers["pair"] = [](IParseHandle& h, TLocation loc) -> TAstTask {
        auto left = co_await h.Expr();
        auto right = co_await h.Expr();
        co_await h.Take(')');
        co_return MakeIntrusive<TPairExpr>(std::move(left), std::move(right));
    };
    return p;
}

TExprPtr Parse(const std::string& src) {
    std::istringstream in(src);
    TTokenStream ts(in);
    auto result = MakeParser().Parse(ts);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().ToString());
    return result.value_or(nullptr);
}

} // namespace

TEST(ParserCustomNodes, TagSimple) {
    auto expr = Parse(R"((tag "hello" x))");
    ASSERT_NE(expr, nullptr);
    auto* tag = dynamic_cast<TTagExpr*>(expr.Get());
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(tag->Label, "hello");
    auto* child = dynamic_cast<TIdentExpr*>(tag->Child.Get());
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->Name, "x");
}

TEST(ParserCustomNodes, PairSimple) {
    auto expr = Parse("(pair a b)");
    ASSERT_NE(expr, nullptr);
    auto* pair = dynamic_cast<TPairExpr*>(expr.Get());
    ASSERT_NE(pair, nullptr);
    EXPECT_EQ(dynamic_cast<TIdentExpr*>(pair->Left.Get())->Name, "a");
    EXPECT_EQ(dynamic_cast<TIdentExpr*>(pair->Right.Get())->Name, "b");
}

TEST(ParserCustomNodes, NestedCustomNodes) {
    auto expr = Parse(R"((tag "wrap" (pair x y)))");
    ASSERT_NE(expr, nullptr);
    auto* tag = dynamic_cast<TTagExpr*>(expr.Get());
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(tag->Label, "wrap");
    auto* pair = dynamic_cast<TPairExpr*>(tag->Child.Get());
    ASSERT_NE(pair, nullptr);
}

TEST(ParserCustomNodes, UnknownFormFallsBackToBinaryOp) {
    auto expr = Parse("(+ a b)");
    ASSERT_NE(expr, nullptr);
    EXPECT_NE(dynamic_cast<TBinaryExpr*>(expr.Get()), nullptr);
}

TEST(CoreParser, CanonicalizesXorCompatibilityAlias) {
    auto expr = Parse("(xor a b)");

    ASSERT_NE(expr, nullptr);
    auto binary = dynamic_cast<TBinaryExpr*>(expr.Get());
    ASSERT_NE(binary, nullptr);
    EXPECT_EQ(binary->Operator, TOperator("^"));
    EXPECT_EQ(PrintAst(expr, MakeOptions()), "(^ a b)");
}

TEST(ParserCustomNodes, UnknownFormErrors) {
    std::istringstream in("(mystery a b c)");
    TTokenStream ts(in);
    auto result = MakeParser().Parse(ts);
    EXPECT_FALSE(result.has_value());
}

TEST(LifetimeNodes, UnaryFormsPrintCompactly) {
    auto borrowed = MakeIntrusive<TBorrowExpr>(TLocation{}, MakeIdent("s"));
    auto retained = MakeIntrusive<TRetainExpr>(TLocation{}, borrowed);
    auto ownedLiteral = MakeIntrusive<TOwnLiteralExpr>(TLocation{}, MakeIdent("literal"));
    auto moved = MakeIntrusive<TMoveExpr>(TLocation{}, MakeIdent("owned"));

    EXPECT_EQ(PrintAst(retained, MakeOptions(false)), "(retain (borrow s))");
    EXPECT_EQ(PrintAst(ownedLiteral, MakeOptions(false)), "(own-literal literal)");
    EXPECT_EQ(PrintAst(moved, MakeOptions(false)), "(move owned)");
}

TEST(LifetimeNodes, DestroyAndReplacePrintCompactly) {
    auto destroy = MakeIntrusive<TDestroyExpr>(
        TLocation{},
        MakeIdent("array"), NQumir::NTest::TestTypes(),
        MakeIdent("array_size"));
    auto replace = MakeIntrusive<TReplaceExpr>(
        TLocation{},
        MakeIdent("target"),
        MakeIntrusive<TRetainExpr>(
            TLocation{},
            MakeIntrusive<TBorrowExpr>(TLocation{}, MakeIdent("source"))), NQumir::NTest::TestTypes());

    EXPECT_EQ(PrintAst(destroy, MakeOptions(false)), "(destroy array array_size)");
    EXPECT_EQ(
        PrintAst(replace, MakeOptions(false)),
        "(replace target (retain (borrow source)))");
}

TEST(LifetimeNodes, CleanupFormsPreserveStoredOrder) {
    std::vector<TExprPtr> cleanups {
        MakeIntrusive<TDestroyExpr>(TLocation{}, MakeIdent("second"), NQumir::NTest::TestTypes()),
        MakeIntrusive<TDestroyExpr>(TLocation{}, MakeIdent("first"), NQumir::NTest::TestTypes()),
    };
    auto exit = MakeIntrusive<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Return,
        MakeIntrusive<TRetainExpr>(
            TLocation{},
            MakeIntrusive<TBorrowExpr>(TLocation{}, MakeIdent("result"))),
        auto(cleanups), NQumir::NTest::TestTypes());
    auto global = MakeIntrusive<TGlobalCleanupExpr>(TLocation{}, auto(cleanups), NQumir::NTest::TestTypes());
    auto breakExit = MakeIntrusive<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Break,
        nullptr,
        auto(cleanups), NQumir::NTest::TestTypes());
    auto continueExit = MakeIntrusive<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Continue,
        nullptr,
        std::vector<TExprPtr>{}, NQumir::NTest::TestTypes());

    EXPECT_EQ(
        PrintAst(exit, MakeOptions(false)),
        "(cleanup-exit (return (retain (borrow result))) (destroy second) (destroy first))");
    EXPECT_EQ(
        PrintAst(global, MakeOptions(false)),
        "(cleanup-global (destroy second) (destroy first))");
    EXPECT_EQ(
        PrintAst(breakExit, MakeOptions(false)),
        "(cleanup-exit (break) (destroy second) (destroy first))");
    EXPECT_EQ(PrintAst(continueExit, MakeOptions(false)), "(cleanup-exit (continue))");
}

TEST(LifetimeNodes, ReplaceChildrenUseStructuralOrder) {
    auto target = MakeIdent("target");
    auto value = MakeIdent("value");
    auto replace = MakeIntrusive<TReplaceExpr>(TLocation{}, target, value, NQumir::NTest::TestTypes());

    const auto children = replace->Children();
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0], target);
    EXPECT_EQ(children[1], value);

    const auto mutableChildren = replace->MutableChildren();
    ASSERT_EQ(mutableChildren.size(), 2u);
    EXPECT_EQ(mutableChildren[0], &replace->Target);
    EXPECT_EQ(mutableChildren[1], &replace->Value);
}

TEST(LifetimeNodes, DestroyChildrenIncludeOptionalAux) {
    auto value = MakeIdent("value");
    auto aux = MakeIdent("aux");
    auto withoutAux = MakeIntrusive<TDestroyExpr>(TLocation{}, value, NQumir::NTest::TestTypes());
    auto withAux = MakeIntrusive<TDestroyExpr>(TLocation{}, value, NQumir::NTest::TestTypes(), aux);

    ASSERT_EQ(withoutAux->Children().size(), 1u);
    ASSERT_EQ(withAux->Children().size(), 2u);
    EXPECT_EQ(withAux->Children()[0], value);
    EXPECT_EQ(withAux->Children()[1], aux);
    EXPECT_EQ(withAux->MutableChildren()[0], &withAux->Value);
    EXPECT_EQ(withAux->MutableChildren()[1], &withAux->Aux);
}

TEST(LifetimeNodes, CleanupChildrenPutValueBeforeOrderedCleanups) {
    auto value = MakeIdent("value");
    auto cleanup1 = MakeIdent("cleanup1");
    auto cleanup2 = MakeIdent("cleanup2");
    auto exit = MakeIntrusive<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Return,
        value,
        std::vector<TExprPtr>{cleanup1, cleanup2}, NQumir::NTest::TestTypes());

    const auto children = exit->Children();
    ASSERT_EQ(children.size(), 3u);
    EXPECT_EQ(children[0], value);
    EXPECT_EQ(children[1], cleanup1);
    EXPECT_EQ(children[2], cleanup2);

    auto replacement = MakeIdent("replacement");
    auto mutableChildren = exit->MutableChildren();
    ASSERT_EQ(mutableChildren.size(), 3u);
    *mutableChildren[1] = replacement;
    EXPECT_EQ(exit->Cleanups[0], replacement);

    auto global = MakeIntrusive<TGlobalCleanupExpr>(
        TLocation{},
        std::vector<TExprPtr>{cleanup2, cleanup1}, NQumir::NTest::TestTypes());
    EXPECT_EQ(global->Children(), (std::vector<TExprPtr>{cleanup2, cleanup1}));
}

namespace {

// Custom type: <named Opt [T]>
struct TOptType : TType {
    static constexpr const char* TypeId = "Opt";

    TTypePtr Inner = nullptr;

    const std::string_view TypeName() const override { return TypeId; }

private:
    friend class NQumir::NAst::TTypeFactory;

    explicit TOptType(TTypePtr inner)
        : Inner(std::move(inner))
    {}
};

// A host type as a named type with value args: <named Dec [precision scale]>
TTypePtr MakeDec(int precision, int scale) {
    return NQumir::NTest::TestTypes().MakeNamed("Dec", nullptr, {
        TGenericArg::ValueArg(std::to_string(precision)),
        TGenericArg::ValueArg(std::to_string(scale)),
    });
}

TPrintOptions MakeTypePrintOptions() {
    TPrintOptions opts;
    opts.TypePrinters[TOptType::TypeId] = [](const TType& type, TPrinter& p, int level) {
        p.PrintNamedType("Opt", {TGenericArg::TypeArg(static_cast<const TOptType&>(type).Inner)}, nullptr, level);
    };
    return opts;
}

TTypeParserMap MakeTypeParsers() {
    TTypeParserMap parsers;
    parsers["Opt"] = [](TTypeFactory& types, std::vector<TGenericArg> args) -> std::expected<TTypePtr, std::string> {
        if (args.size() != 1 || args[0].Kind != TGenericArg::EKind::Type) {
            return std::unexpected("Opt expects one type argument");
        }
        return types.MakeCustom<TOptType>(args[0].Type);
    };
    parsers["Dec"] = [](TTypeFactory& types, std::vector<TGenericArg> args) -> std::expected<TTypePtr, std::string> {
        // A bare Dec means the default precision and scale.
        if (args.empty()) {
            args = {TGenericArg::ValueArg("18"), TGenericArg::ValueArg("0")};
        }
        if (args.size() != 2 || args[0].Kind != TGenericArg::EKind::Value || args[1].Kind != TGenericArg::EKind::Value) {
            return std::unexpected("Dec expects two value arguments");
        }
        return types.MakeNamed("Dec", nullptr, std::move(args));
    };
    return parsers;
}

std::expected<TTypePtr, TError> ParseTypeText(const std::string& src, const TTypeParserMap& parsers) {
    std::istringstream in(src);
    TTokenStream ts(in);
    return ParseType(ts, parsers, NQumir::NTest::TestTypes());
}

TTypePtr MakeNestedType() {
    auto dec = MakeDec(18, 4);
    auto opt = NQumir::NTest::TestTypes().MakeCustom<TOptType>(dec);
    auto optOpt = NQumir::NTest::TestTypes().MakeCustom<TOptType>(opt);
    return NQumir::NTest::TestTypes().MakeStruct(std::vector<std::pair<std::string, TTypePtr>>{
        {"a", optOpt},
        {"b", dec},
        {"c", NQumir::NTest::TestTypes().MakeInteger(TIntegerType::I64)},
    });
}

} // namespace

TEST(TypeExtensions, PrintWithoutPrinterThrows) {
    EXPECT_THROW(PrintType(NQumir::NTest::TestTypes().MakeCustom<TOptType>(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::I64))), std::runtime_error);
}

TEST(TypeExtensions, PrintCustomTypes) {
    auto opts = MakeTypePrintOptions();
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeCustom<TOptType>(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::I64)), opts),
              "<named Opt [i64]>");
    EXPECT_EQ(PrintType(MakeDec(18, 4), opts), "<named Dec [18 4]>");
}

TEST(TypeExtensions, ParseWithoutParsersKeepsNamedType) {
    auto type = ParseTypeText("<named Opt [i64]>", {});
    ASSERT_TRUE(type.has_value());
    EXPECT_TRUE(TMaybeType<TNamedType>(*type));
    EXPECT_FALSE(DynCast<TOptType>(*type));
}

TEST(TypeExtensions, BareNamedTypeConsultsParsers) {
    TPrintOptions opts;
    opts.ShortNamedTypes.insert("Dec");
    auto text = PrintType(NQumir::NTest::TestTypes().MakeNamed("Dec"), opts);
    EXPECT_EQ(text, "Dec");
    auto parsed = ParseTypeText(text, MakeTypeParsers());
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();
    EXPECT_EQ(*parsed, MakeDec(18, 0));
    auto plain = ParseTypeText(text, {});
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(*plain, NQumir::NTest::TestTypes().MakeNamed("Dec"));
}

TEST(TypeExtensions, RoundTripNested) {
    auto opts = MakeTypePrintOptions();
    auto parsers = MakeTypeParsers();
    auto text = PrintType(MakeNestedType(), opts);
    auto parsed = ParseTypeText(text, parsers);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();
    EXPECT_EQ(PrintType(*parsed, opts), text);

    auto st = TMaybeType<TStructType>(*parsed);
    ASSERT_TRUE(st);
    auto outer = DynCast<TOptType>(st.Cast()->Fields[0].second);
    ASSERT_TRUE(outer);
    auto inner = DynCast<TOptType>(outer->Inner);
    ASSERT_TRUE(inner);
    // A named host type is interned: parsing gives back the very same object.
    EXPECT_EQ(inner->Inner, MakeDec(18, 4));
    EXPECT_EQ(st.Cast()->Fields[1].second, MakeDec(18, 4));
}

TEST(TypeExtensions, ParserTypeParsersApplyInsideExpressions) {
    auto parser = MakeParser();
    parser.TypeParsers = MakeTypeParsers();
    std::istringstream in("(: x <named Opt [i64]>)");
    TTokenStream ts(in);
    auto expr = parser.Parse(ts);
    ASSERT_TRUE(expr.has_value()) << expr.error().ToString();
    EXPECT_TRUE(DynCast<TOptType>((*expr)->Type));
}

TEST(TypeExtensions, ParserErrorFromBuilder) {
    auto parsed = ParseTypeText("<named Opt [1 2]>", MakeTypeParsers());
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().ToString().find("Opt expects one type argument"), std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
