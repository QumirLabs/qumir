#include <gtest/gtest.h>

#include <qumir/location.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
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
    return std::make_shared<TIdentExpr>(TLocation{}, std::move(name));
}

} // namespace

TEST(PrinterCustomNodes, TagCompact) {
    auto expr = std::make_shared<TTagExpr>("hello", MakeIdent("x"));
    EXPECT_EQ(PrintAst(expr, MakeOptions()), "(tag \"hello\" x)");
}

TEST(PrinterNumbers, IntegralFloatKeepsItsLiteralType) {
    EXPECT_EQ(PrintAst(std::make_shared<TNumberExpr>(TLocation{}, 1.0)), "1.0");
    EXPECT_EQ(PrintAst(std::make_shared<TNumberExpr>(TLocation{}, -0.0)), "-0.0");
    EXPECT_EQ(PrintAst(std::make_shared<TNumberExpr>(TLocation{}, 1e20)), "1e+20");
}

TEST(PrinterCustomNodes, PairCompact) {
    auto expr = std::make_shared<TPairExpr>(MakeIdent("a"), MakeIdent("b"));
    EXPECT_EQ(PrintAst(expr, MakeOptions()), "(pair a b)");
}

TEST(PrinterCustomNodes, NestedCustomNodes) {
    auto inner = std::make_shared<TPairExpr>(MakeIdent("x"), MakeIdent("y"));
    auto outer = std::make_shared<TTagExpr>("wrap", inner);
    EXPECT_EQ(PrintAst(outer, MakeOptions()), "(tag \"wrap\" (pair x y))");
}

TEST(PrinterCustomNodes, UnknownNodeThrows) {
    struct TUnknown : TExpr {
        const std::string_view NodeName() const override { return "Unknown"; }
        void Accept(IVisitor& v) override { v.VisitOtherwise(*this); }
    };
    auto expr = std::make_shared<TUnknown>();
    TPrintOptions opts;
    EXPECT_THROW(PrintAst(expr, opts), std::runtime_error);
}

namespace {

TParser MakeParser() {
    TParser p;
    p.NodeParsers["tag"] = [](IParseHandle& h, TLocation loc) -> TAstTask {
        auto tok = h.Next();
        if (tok.Type != TToken::String) {
            co_return IParseHandle::MakeError(tok, "expected string label for tag");
        }
        auto child = co_await h.Expr();
        co_await h.Take(')');
        co_return std::make_shared<TTagExpr>(tok.Name, std::move(child));
    };
    p.NodeParsers["pair"] = [](IParseHandle& h, TLocation loc) -> TAstTask {
        auto left = co_await h.Expr();
        auto right = co_await h.Expr();
        co_await h.Take(')');
        co_return std::make_shared<TPairExpr>(std::move(left), std::move(right));
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
    auto* tag = dynamic_cast<TTagExpr*>(expr.get());
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(tag->Label, "hello");
    auto* child = dynamic_cast<TIdentExpr*>(tag->Child.get());
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->Name, "x");
}

TEST(ParserCustomNodes, PairSimple) {
    auto expr = Parse("(pair a b)");
    ASSERT_NE(expr, nullptr);
    auto* pair = dynamic_cast<TPairExpr*>(expr.get());
    ASSERT_NE(pair, nullptr);
    EXPECT_EQ(dynamic_cast<TIdentExpr*>(pair->Left.get())->Name, "a");
    EXPECT_EQ(dynamic_cast<TIdentExpr*>(pair->Right.get())->Name, "b");
}

TEST(ParserCustomNodes, NestedCustomNodes) {
    auto expr = Parse(R"((tag "wrap" (pair x y)))");
    ASSERT_NE(expr, nullptr);
    auto* tag = dynamic_cast<TTagExpr*>(expr.get());
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(tag->Label, "wrap");
    auto* pair = dynamic_cast<TPairExpr*>(tag->Child.get());
    ASSERT_NE(pair, nullptr);
}

TEST(ParserCustomNodes, UnknownFormFallsBackToBinaryOp) {
    auto expr = Parse("(+ a b)");
    ASSERT_NE(expr, nullptr);
    EXPECT_NE(dynamic_cast<TBinaryExpr*>(expr.get()), nullptr);
}

TEST(CoreParser, CanonicalizesXorCompatibilityAlias) {
    auto expr = Parse("(xor a b)");

    ASSERT_NE(expr, nullptr);
    auto binary = dynamic_cast<TBinaryExpr*>(expr.get());
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
    auto borrowed = std::make_shared<TBorrowExpr>(TLocation{}, MakeIdent("s"));
    auto retained = std::make_shared<TRetainExpr>(TLocation{}, borrowed);
    auto ownedLiteral = std::make_shared<TOwnLiteralExpr>(TLocation{}, MakeIdent("literal"));
    auto moved = std::make_shared<TMoveExpr>(TLocation{}, MakeIdent("owned"));

    EXPECT_EQ(PrintAst(retained, MakeOptions(false)), "(retain (borrow s))");
    EXPECT_EQ(PrintAst(ownedLiteral, MakeOptions(false)), "(own-literal literal)");
    EXPECT_EQ(PrintAst(moved, MakeOptions(false)), "(move owned)");
}

TEST(LifetimeNodes, DestroyAndReplacePrintCompactly) {
    auto destroy = std::make_shared<TDestroyExpr>(
        TLocation{},
        MakeIdent("array"),
        MakeIdent("array_size"));
    auto replace = std::make_shared<TReplaceExpr>(
        TLocation{},
        MakeIdent("target"),
        std::make_shared<TRetainExpr>(
            TLocation{},
            std::make_shared<TBorrowExpr>(TLocation{}, MakeIdent("source"))));

    EXPECT_EQ(PrintAst(destroy, MakeOptions(false)), "(destroy array array_size)");
    EXPECT_EQ(
        PrintAst(replace, MakeOptions(false)),
        "(replace target (retain (borrow source)))");
}

TEST(LifetimeNodes, CleanupFormsPreserveStoredOrder) {
    std::vector<TExprPtr> cleanups {
        std::make_shared<TDestroyExpr>(TLocation{}, MakeIdent("second")),
        std::make_shared<TDestroyExpr>(TLocation{}, MakeIdent("first")),
    };
    auto exit = std::make_shared<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Return,
        std::make_shared<TRetainExpr>(
            TLocation{},
            std::make_shared<TBorrowExpr>(TLocation{}, MakeIdent("result"))),
        cleanups);
    auto global = std::make_shared<TGlobalCleanupExpr>(TLocation{}, cleanups);
    auto breakExit = std::make_shared<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Break,
        nullptr,
        cleanups);
    auto continueExit = std::make_shared<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Continue,
        nullptr,
        std::vector<TExprPtr>{});

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
    auto replace = std::make_shared<TReplaceExpr>(TLocation{}, target, value);

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
    auto withoutAux = std::make_shared<TDestroyExpr>(TLocation{}, value);
    auto withAux = std::make_shared<TDestroyExpr>(TLocation{}, value, aux);

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
    auto exit = std::make_shared<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Return,
        value,
        std::vector<TExprPtr>{cleanup1, cleanup2});

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

    auto global = std::make_shared<TGlobalCleanupExpr>(
        TLocation{},
        std::vector<TExprPtr>{cleanup2, cleanup1});
    EXPECT_EQ(global->Children(), (std::vector<TExprPtr>{cleanup2, cleanup1}));
}

namespace {

// Custom type: <named Opt [T]>
struct TOptType : TType {
    static constexpr const char* TypeId = "Opt";

    TTypePtr Inner;

    explicit TOptType(TTypePtr inner)
        : Inner(std::move(inner))
    {}

    const std::string_view TypeName() const override { return TypeId; }
};

// Custom named type with value args: <named Dec [precision scale]>
struct TDecType : TNamedType {
    int Precision;
    int Scale;

    TDecType(int precision, int scale)
        : TNamedType("Dec", nullptr, {
              TGenericArg::ValueArg(std::to_string(precision)),
              TGenericArg::ValueArg(std::to_string(scale)),
          })
        , Precision(precision)
        , Scale(scale)
    {}
};

TPrintOptions MakeTypePrintOptions() {
    TPrintOptions opts;
    opts.TypePrinters[TOptType::TypeId] = [](TType& type, TPrinter& p, int level) {
        TNamedType named("Opt", nullptr, {TGenericArg::TypeArg(static_cast<TOptType&>(type).Inner)});
        p.PrintType(std::make_shared<TNamedType>(std::move(named)), level);
    };
    return opts;
}

TTypeParserMap MakeTypeParsers() {
    TTypeParserMap parsers;
    parsers["Opt"] = [](std::vector<TGenericArg> args) -> std::expected<TTypePtr, std::string> {
        if (args.size() != 1 || args[0].Kind != TGenericArg::EKind::Type) {
            return std::unexpected("Opt expects one type argument");
        }
        return std::make_shared<TOptType>(std::move(args[0].Type));
    };
    parsers["Dec"] = [](std::vector<TGenericArg> args) -> std::expected<TTypePtr, std::string> {
        if (args.size() != 2 || args[0].Kind != TGenericArg::EKind::Value || args[1].Kind != TGenericArg::EKind::Value) {
            return std::unexpected("Dec expects two value arguments");
        }
        return std::make_shared<TDecType>(std::stoi(args[0].Value), std::stoi(args[1].Value));
    };
    return parsers;
}

std::expected<TTypePtr, TError> ParseTypeText(const std::string& src, const TTypeParserMap& parsers) {
    std::istringstream in(src);
    TTokenStream ts(in);
    return ParseType(ts, parsers);
}

TTypePtr MakeNestedType() {
    auto dec = std::make_shared<TDecType>(18, 4);
    auto opt = std::make_shared<TOptType>(dec);
    auto optOpt = std::make_shared<TOptType>(opt);
    return std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{
        {"a", optOpt},
        {"b", dec},
        {"c", std::make_shared<TIntegerType>(TIntegerType::I64)},
    });
}

} // namespace

TEST(TypeExtensions, PrintWithoutPrinterThrows) {
    EXPECT_THROW(PrintType(std::make_shared<TOptType>(std::make_shared<TIntegerType>(TIntegerType::I64))), std::runtime_error);
}

TEST(TypeExtensions, PrintCustomTypes) {
    auto opts = MakeTypePrintOptions();
    EXPECT_EQ(PrintType(std::make_shared<TOptType>(std::make_shared<TIntegerType>(TIntegerType::I64)), opts),
              "<named Opt [i64]>");
    EXPECT_EQ(PrintType(std::make_shared<TDecType>(18, 4), opts), "<named Dec [18 4]>");
}

TEST(TypeExtensions, ParseWithoutParsersKeepsNamedType) {
    auto type = ParseTypeText("<named Opt [i64]>", {});
    ASSERT_TRUE(type.has_value());
    EXPECT_TRUE(TMaybeType<TNamedType>(*type));
    EXPECT_FALSE(std::dynamic_pointer_cast<TOptType>(*type));
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
    auto outer = std::dynamic_pointer_cast<TOptType>(st.Cast()->Fields[0].second);
    ASSERT_TRUE(outer);
    auto inner = std::dynamic_pointer_cast<TOptType>(outer->Inner);
    ASSERT_TRUE(inner);
    auto dec = std::dynamic_pointer_cast<TDecType>(inner->Inner);
    ASSERT_TRUE(dec);
    EXPECT_EQ(dec->Precision, 18);
    EXPECT_EQ(dec->Scale, 4);
    EXPECT_TRUE(std::dynamic_pointer_cast<TDecType>(st.Cast()->Fields[1].second));
}

TEST(TypeExtensions, ParserTypeParsersApplyInsideExpressions) {
    auto parser = MakeParser();
    parser.TypeParsers = MakeTypeParsers();
    std::istringstream in("(: x <named Opt [i64]>)");
    TTokenStream ts(in);
    auto expr = parser.Parse(ts);
    ASSERT_TRUE(expr.has_value()) << expr.error().ToString();
    EXPECT_TRUE(std::dynamic_pointer_cast<TOptType>((*expr)->Type));
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
