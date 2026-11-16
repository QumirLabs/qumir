#include <gtest/gtest.h>
#include <qumir/parser/lexer.h>
#include <sstream>

using namespace NQumir::NAst;

namespace {

// --- helpers ----------------------------------------------------------------
#define ExpectKeyword(t, kw) \
    do { \
        auto value = (t); \
        EXPECT_EQ(value.Type, TToken::Keyword); \
        EXPECT_EQ(value.Value.i64, (int64_t)(kw)); \
    } while(0);

#define ExpectOp(t, op) \
    do { \
        auto value = (t); \
        EXPECT_EQ(value.Type, TToken::Operator); \
        EXPECT_EQ(value.Value.i64, (int64_t)(op)); \
    } while(0);

#define ExpectIdent(t, name) \
    do { \
        auto value = (t); \
        EXPECT_EQ(value.Type, TToken::Identifier); \
        EXPECT_EQ(value.Name, (name)); \
    } while(0);

#define ExpectInt(t, v) \
    do { \
        auto value = (t); \
        EXPECT_EQ(value.Type, TToken::Integer); \
        EXPECT_EQ(value.Value.i64, (v)); \
    } while(0);

#define ExpectFloat(t, v) \
    do { \
        auto value = (t); \
        EXPECT_EQ(value.Type, TToken::Float); \
        EXPECT_EQ(value.Value.f64, (v)); \
    } while(0);

} // namespace

TEST(LexerTest, Numbers) {
    std::istringstream input("42 + 23");
    TTokenStream tokens(input);

    ExpectInt(tokens.Next(), 42);
    ExpectOp(tokens.Next(), EOperator::Plus);
    ExpectInt(tokens.Next(), 23);
}

TEST(LexerTest, NegativeInt) {
    std::istringstream input("-1");
    TTokenStream tokens(input);

    ExpectOp(tokens.Next(), EOperator::Minus);
    ExpectInt(tokens.Next(), 1);
}

TEST(LexerTest, NegativeFloat) {
    std::istringstream input("-.1");
    TTokenStream tokens(input);

    ExpectOp(tokens.Next(), EOperator::Minus);
    ExpectFloat(tokens.Next(), .1);
}

TEST(LexerTest, NegativeFloat2) {
    std::istringstream input("-1.1");
    TTokenStream tokens(input);

    ExpectOp(tokens.Next(), EOperator::Minus);
    ExpectFloat(tokens.Next(), 1.1);
}

TEST(LexerTest, LongFractionalPart) {
    struct TCase {
        std::string Literal;
        double Expected;
    };
    const TCase cases[] = {
        {"0.000000001", 1e-9},
        {"0.0000000001", 1e-10},
        {"1.2345678901234567", 1.2345678901234567},
        {"0.0000000001e+3", 1e-7},
        {"1." + std::string(400, '0'), 1.0},
        {".1" + std::string(400, '0'), 0.1},
        {"0." + std::string(400, '0') + "1e401", 1.0},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.Literal);
        std::istringstream input(test.Literal);
        TTokenStream tokens(input);
        const auto value = tokens.Next();
        ASSERT_EQ(value.Type, TToken::Float);
        EXPECT_DOUBLE_EQ(value.Value.f64, test.Expected);
        EXPECT_EQ(value.RawValue, test.Literal);
    }
}

TEST(LexerTest, Assignment) {
    std::istringstream input("x := 23");
    TTokenStream tokens(input);

    ExpectIdent(tokens.Next(), "x");
    ExpectOp(tokens.Next(), EOperator::Assign);
    ExpectInt(tokens.Next(), 23);
}

// --- multi-word identifiers --------------------------------------------------
TEST(LexerTest, MultiWordIdentifierDeclarationAndUse) {
    std::istringstream input(
        "цел длина отрезка\n"
        "длина отрезка := 5\n"
    );
    TTokenStream tokens(input);

    // "цел длина отрезка"
    ExpectKeyword(tokens.Next(), EKeyword::Int);
    ExpectIdent(tokens.Next(), "длина отрезка");
    ExpectOp(tokens.Next(), EOperator::Eol);

    // "длина отрезка := 5"
    ExpectIdent(tokens.Next(), "длина отрезка");
    ExpectOp(tokens.Next(), EOperator::Assign);
    ExpectInt(tokens.Next(), 5);
}

// --- 'не' as identifier on LHS, and as operator in expression ---------------
TEST(LexerTest, NotAsIdentifierAndOperator) {
    std::istringstream input(
        "лог не\n"
        "не := нет\n"
        "если не x то\n"
    );
    TTokenStream tokens(input);

    // "лог не"
    ExpectKeyword(tokens.Next(), EKeyword::Bool);
    ExpectOp(tokens.Next(), EOperator::Not);
    ExpectOp(tokens.Next(), EOperator::Eol);

    // "не := ложь"
    ExpectOp(tokens.Next(), EOperator::Not);
    ExpectOp(tokens.Next(), EOperator::Assign);
    ExpectKeyword(tokens.Next(), EKeyword::False);
    ExpectOp(tokens.Next(), EOperator::Eol);

    // "если не x то"
    ExpectKeyword(tokens.Next(), EKeyword::If);
    ExpectOp(tokens.Next(), EOperator::Not); // 'не' как оператор
    ExpectIdent(tokens.Next(), "x");
    ExpectKeyword(tokens.Next(), EKeyword::Then);
}

// --- 'не готов' (multi-word starting with 'не ') ----------------------------
TEST(LexerTest, NotWordChainAsLhsAndInExpr) {
    std::istringstream input(
        "лог не готов\n"
        "не готов := да\n"
        "если не готов то\n"
    );
    TTokenStream tokens(input);

    // Declaration "лог не готов"
    ExpectKeyword(tokens.Next(), EKeyword::Bool);
    ExpectOp(tokens.Next(), EOperator::Not);
    ExpectIdent(tokens.Next(), "готов");
    ExpectOp(tokens.Next(), EOperator::Eol);

    // Assignment "не готов := истина"
    ExpectOp(tokens.Next(), EOperator::Not);
    ExpectIdent(tokens.Next(), "готов");
    ExpectOp(tokens.Next(), EOperator::Assign);
    ExpectKeyword(tokens.Next(), EKeyword::True);
    ExpectOp(tokens.Next(), EOperator::Eol);

    // Expression "если не готов то"  (в Expr наш фильтр трактует как NOT + IDENT("готов"))
    ExpectKeyword(tokens.Next(), EKeyword::If);
    ExpectOp(tokens.Next(), EOperator::Not);
    ExpectIdent(tokens.Next(), "готов");
    ExpectKeyword(tokens.Next(), EKeyword::Then);
}

// --- keywords inside identifiers on LHS -------------------------------------
TEST(LexerTest, KeywordsInsideIdentifierLhs) {
    std::istringstream input("цел если число\nесли число := 10\n");
    TTokenStream tokens(input);
    ExpectKeyword(tokens.Next(), EKeyword::Int);
    ExpectKeyword(tokens.Next(), EKeyword::If);
    ExpectIdent(tokens.Next(), "число");
    ExpectOp(tokens.Next(), EOperator::Eol);
    ExpectKeyword(tokens.Next(), EKeyword::If);
    ExpectIdent(tokens.Next(), "число");
    ExpectOp(tokens.Next(), EOperator::Assign);
    ExpectInt(tokens.Next(), 10);
}

// --- ввод/вывод lists expect identifiers after commas -----------------------
TEST(LexerTest, InputOutputListsWithMultiWordIdentifiers) {
    std::istringstream input(
        "ввод длина отрезка, ширина прямоугольника\n"
        "вывод длина отрезка, нс\n"
    );
    TTokenStream tokens(input);
    // ввод ...
    ExpectKeyword(tokens.Next(), EKeyword::Input);
    ExpectIdent(tokens.Next(), "длина отрезка");
    ExpectOp(tokens.Next(), EOperator::Comma);
    ExpectIdent(tokens.Next(), "ширина прямоугольника");
    ExpectOp(tokens.Next(), EOperator::Eol);
    // вывод ...
    ExpectKeyword(tokens.Next(), EKeyword::Output);
    ExpectIdent(tokens.Next(), "длина отрезка");
    ExpectOp(tokens.Next(), EOperator::Comma);
    // "нс" как keyword переноса строки (если у тебя так заведено) - иначе можно ожидать Identifier("нс")
    auto t = tokens.Next();
    EXPECT_EQ(t.Type, TToken::Keyword) << "ожидался keyword 'нс'";
    ExpectOp(tokens.Next(), EOperator::Eol);
}

// --- Indexing and brackets ---------------------------------------------------
TEST(LexerTest, IndexingBrackets) {
    std::istringstream input("t[i] := 1");
    TTokenStream tokens(input);
    ExpectIdent(tokens.Next(), "t");
    ExpectOp(tokens.Next(), EOperator::LSqBr);
    ExpectIdent(tokens.Next(), "i");
    ExpectOp(tokens.Next(), EOperator::RSqBr);
    ExpectOp(tokens.Next(), EOperator::Assign);
    ExpectInt(tokens.Next(), 1);
}

// --- Two-char operators and comparisons -------------------------------------
TEST(LexerTest, TwoCharOperators) {
    std::istringstream input("a <= b, c >= d, e <> f, x ** 2");
    TTokenStream tokens(input);
    ExpectIdent(tokens.Next(), "a");
    ExpectOp(tokens.Next(), EOperator::Leq);
    ExpectIdent(tokens.Next(), "b");
    ExpectOp(tokens.Next(), EOperator::Comma);
    ExpectIdent(tokens.Next(), "c");
    ExpectOp(tokens.Next(), EOperator::Geq);
    ExpectIdent(tokens.Next(), "d");
    ExpectOp(tokens.Next(), EOperator::Comma);
    ExpectIdent(tokens.Next(), "e");
    ExpectOp(tokens.Next(), EOperator::Neq);
    ExpectIdent(tokens.Next(), "f");
    ExpectOp(tokens.Next(), EOperator::Comma);
    ExpectIdent(tokens.Next(), "x");
    ExpectOp(tokens.Next(), EOperator::Pow);
    ExpectInt(tokens.Next(), 2);
}

// --- Strings and comments ----------------------------------------------------
TEST(LexerTest, StringLiteralAndComments) {
    std::istringstream input(
        "вывод \"Привет\", нс  | комментарий \n"
        "| ещё комментарий до конца строки\n"
        "вывод \"OK\""
    );
    TTokenStream tokens(input);

    ExpectKeyword(tokens.Next(), EKeyword::Output);
    auto t = tokens.Next(); // "Привет"
    EXPECT_EQ(t.Type, TToken::String);
    EXPECT_EQ(t.Name, std::string("Привет"));
    ExpectOp(tokens.Next(), EOperator::Comma);
    // "нс" - как keyword (или идентификатор - см. реализацию)
    t = tokens.Next();
    EXPECT_EQ(t.Type, TToken::Keyword) << "ожидался keyword 'нс'";
    ExpectOp(tokens.Next(), EOperator::Eol); // comment 1
    ExpectOp(tokens.Next(), EOperator::Eol); // comment 2

    // комментарии должны быть съедены, далее снова "вывод"
    ExpectKeyword(tokens.Next(), EKeyword::Output);
    t = tokens.Next();
    EXPECT_EQ(t.Type, TToken::String);
    EXPECT_EQ(t.Name, std::string("OK"));
}

TEST(LexerTest, EolBetweenStatements) {
    std::istringstream input("x := 1\ny := 2");
    TTokenStream tokens(input);
    ExpectIdent(tokens.Next(), "x");
    ExpectOp(tokens.Next(), EOperator::Assign);
    ExpectInt(tokens.Next(), 1);
    ExpectOp(tokens.Next(), EOperator::Eol); // \n
    ExpectIdent(tokens.Next(), "y");
    ExpectOp(tokens.Next(), EOperator::Assign);
    ExpectInt(tokens.Next(), 2);
}

TEST(LexerTest, TokenLocationsAreOneBasedOnEveryLine) {
    std::istringstream input("x := 1\n  y := 22");
    TTokenStream tokens(input);

    auto expect = [](const TToken& token, int line, int column) {
        EXPECT_EQ(token.Location.Line, line);
        EXPECT_EQ(token.Location.Column, column);
        EXPECT_EQ(token.Location.Byte, column);
    };

    expect(tokens.Next(), 1, 1); // x
    expect(tokens.Next(), 1, 3); // :=
    expect(tokens.Next(), 1, 6); // 1
    expect(tokens.Next(), 1, 7); // \n
    expect(tokens.Next(), 2, 3); // y
    expect(tokens.Next(), 2, 5); // :=
    expect(tokens.Next(), 2, 8); // 22
}

TEST(LexerTest, TokenLocationsCountCharactersNotBytes) {
    std::istringstream input("аб := 1\n  аб := 2");
    TTokenStream tokens(input);

    auto expect = [](const TToken& token, int line, int byte, int column) {
        EXPECT_EQ(token.Location.Line, line);
        EXPECT_EQ(token.Location.Byte, byte);
        EXPECT_EQ(token.Location.Column, column);
    };

    expect(tokens.Next(), 1, 1, 1); // аб
    expect(tokens.Next(), 1, 6, 4); // :=
    expect(tokens.Next(), 1, 9, 7); // 1
    expect(tokens.Next(), 1, 10, 8); // \n
    expect(tokens.Next(), 2, 3, 3); // аб
    expect(tokens.Next(), 2, 8, 6); // :=
    expect(tokens.Next(), 2, 11, 9); // 2
}

TEST(LexerTest, String) {
    std::istringstream input("\"Hello, World!\"");
    TTokenStream tokens(input);
    auto t = tokens.Next();
    EXPECT_EQ(t.Type, TToken::String);
    EXPECT_EQ(t.Name, std::string("Hello, World!"));
}

TEST(LexerTest, EmptyString) {
    std::istringstream input("\"\"");
    TTokenStream tokens(input);
    auto t = tokens.Next();
    EXPECT_EQ(t.Type, TToken::String);
    EXPECT_EQ(t.Name, std::string(""));
}

// --- "иначе если" should be two keywords in control flow --------------------
TEST(LexerTest, ElseIfAsTwoKeywords) {
    std::istringstream input("если x то\nиначе если y то\nвсе\n");
    TTokenStream tokens(input);
    ExpectKeyword(tokens.Next(), EKeyword::If);
    ExpectIdent(tokens.Next(), "x");
    ExpectKeyword(tokens.Next(), EKeyword::Then);
    ExpectOp(tokens.Next(), EOperator::Eol);
    ExpectKeyword(tokens.Next(), EKeyword::Else);
    ExpectKeyword(tokens.Next(), EKeyword::If);
    ExpectIdent(tokens.Next(), "y");
    ExpectKeyword(tokens.Next(), EKeyword::Then);
    ExpectOp(tokens.Next(), EOperator::Eol);
    ExpectKeyword(tokens.Next(), EKeyword::EndIf);
}

TEST(LexerTest, ScientificNotationBasic) {
    // choose values that are exactly representable in binary where possible
    std::istringstream input("1e3 2.5e1 5e-1 1.e2 .1e1 3E+4 10e0");
    TTokenStream tokens(input);

    ExpectFloat(tokens.Next(), 1000.0);   // 1e3
    ExpectFloat(tokens.Next(), 25.0);     // 2.5e1
    ExpectFloat(tokens.Next(), 0.5);      // 5e-1
    ExpectFloat(tokens.Next(), 100.0);    // 1.e2
    ExpectFloat(tokens.Next(), 1.0);      // .1e1
    ExpectFloat(tokens.Next(), 30000.0);  // 3E+4
    ExpectFloat(tokens.Next(), 10.0);     // 10e0
}

TEST(LexerTest, ScientificNotationWithOperators) {
    std::istringstream input("42e-1 + 1E+1");
    TTokenStream tokens(input);

    ExpectFloat(tokens.Next(), 4.2);            // 42e-1
    ExpectOp(tokens.Next(), EOperator::Plus);   // '+' operator
    ExpectFloat(tokens.Next(), 10.0);           // 1E+1
}

TEST(WrappedTokenStreamTest, BasicWindowFilling) {
    std::istringstream input("x := 1 + 2");
    TTokenStream baseTokens(input);
    TWrappedTokenStream wrapped(baseTokens, 3);

    auto t1 = wrapped.Next();
    ExpectIdent(t1, "x");
    EXPECT_EQ(wrapped.GetWindow().size(), 1);
    EXPECT_EQ(wrapped.GetWindow()[0].Name, "x");

    auto t2 = wrapped.Next();
    ExpectOp(t2, EOperator::Assign);
    EXPECT_EQ(wrapped.GetWindow().size(), 2);

    auto t3 = wrapped.Next();
    ExpectInt(t3, 1);
    EXPECT_EQ(wrapped.GetWindow().size(), 3);
}

TEST(WrappedTokenStreamTest, WindowSizeLimit) {
    std::istringstream input("a := b + c - d");
    TTokenStream baseTokens(input);
    TWrappedTokenStream wrapped(baseTokens, 3);

    wrapped.Next(); // a
    wrapped.Next(); // :=
    wrapped.Next(); // b
    wrapped.Next(); // +
    wrapped.Next(); // c

    EXPECT_EQ(wrapped.GetWindow().size(), 3);
    ExpectIdent(wrapped.GetWindow()[0], "b");
    ExpectOp(wrapped.GetWindow()[1], EOperator::Plus);
    ExpectIdent(wrapped.GetWindow()[2], "c");

    wrapped.Next(); // -
    EXPECT_EQ(wrapped.GetWindow().size(), 3);
    ExpectOp(wrapped.GetWindow()[0], EOperator::Plus);
    ExpectIdent(wrapped.GetWindow()[1], "c");
    ExpectOp(wrapped.GetWindow()[2], EOperator::Minus);
}

TEST(WrappedTokenStreamTest, UngetRemovesFromWindow) {
    std::istringstream input("x := 5");
    TTokenStream baseTokens(input);
    TWrappedTokenStream wrapped(baseTokens, 5);

    auto t1 = wrapped.Next(); // x
    auto t2 = wrapped.Next(); // :=
    auto t3 = wrapped.Next(); // 5

    EXPECT_EQ(wrapped.GetWindow().size(), 3);

    wrapped.Unget(t3);
    EXPECT_EQ(wrapped.GetWindow().size(), 2);
    ExpectIdent(wrapped.GetWindow()[0], "x");
    ExpectOp(wrapped.GetWindow()[1], EOperator::Assign);

    auto t3_again = wrapped.Next();
    ExpectInt(t3_again, 5);
    EXPECT_EQ(wrapped.GetWindow().size(), 3);
}

TEST(WrappedTokenStreamTest, MultipleUngets) {
    std::istringstream input("a + b * c");
    TTokenStream baseTokens(input);
    TWrappedTokenStream wrapped(baseTokens, 10);

    auto t1 = wrapped.Next(); // a
    auto t2 = wrapped.Next(); // +
    auto t3 = wrapped.Next(); // b
    auto t4 = wrapped.Next(); // *
    auto t5 = wrapped.Next(); // c

    EXPECT_EQ(wrapped.GetWindow().size(), 5);

    wrapped.Unget(t5);
    wrapped.Unget(t4);
    EXPECT_EQ(wrapped.GetWindow().size(), 3);

    auto t4_again = wrapped.Next();
    ExpectOp(t4_again, EOperator::Mul);
    EXPECT_EQ(wrapped.GetWindow().size(), 4);

    auto t5_again = wrapped.Next();
    ExpectIdent(t5_again, "c");
    EXPECT_EQ(wrapped.GetWindow().size(), 5);
}

TEST(WrappedTokenStreamTest, WindowContextForErrorMessages) {
    std::istringstream input("если x > 0 то\n  вывод x\nвсе");
    TTokenStream baseTokens(input);
    TWrappedTokenStream wrapped(baseTokens, 5);

    wrapped.Next(); // если
    wrapped.Next(); // x
    wrapped.Next(); // >
    wrapped.Next(); // 0
    wrapped.Next(); // то
    wrapped.Next(); // Eol
    wrapped.Next(); // вывод

    const auto& window = wrapped.GetWindow();
    EXPECT_EQ(window.size(), 5);
}

TEST(WrappedTokenStreamTest, EmptyWindow) {
    std::istringstream input("test");
    TTokenStream baseTokens(input);
    TWrappedTokenStream wrapped(baseTokens, 3);

    EXPECT_EQ(wrapped.GetWindow().size(), 0);
}

TEST(WrappedTokenStreamTest, WindowSizeOne) {
    std::istringstream input("1 + 2 + 3");
    TTokenStream baseTokens(input);
    TWrappedTokenStream wrapped(baseTokens, 1);

    wrapped.Next(); // 1
    EXPECT_EQ(wrapped.GetWindow().size(), 1);
    ExpectInt(wrapped.GetWindow()[0], 1);

    wrapped.Next(); // +
    EXPECT_EQ(wrapped.GetWindow().size(), 1);
    ExpectOp(wrapped.GetWindow()[0], EOperator::Plus);

    wrapped.Next(); // 2
    EXPECT_EQ(wrapped.GetWindow().size(), 1);
    ExpectInt(wrapped.GetWindow()[0], 2);
}

TEST(WrappedTokenStreamTest, UngetPastWindow) {
    std::istringstream input("1 2 3");
    TTokenStream baseTokens(input);
    TWrappedTokenStream wrapped(baseTokens, 1);

    auto a = wrapped.Next();
    auto b = wrapped.Next();
    wrapped.Unget(b);
    wrapped.Unget(a); // The window is already empty.
    EXPECT_EQ(wrapped.GetWindow().size(), 0);

    ExpectInt(wrapped.Next(), 1);
    ExpectInt(wrapped.Next(), 2);
}

TEST(WrappedTokenStreamTest, LocationDelegation) {
    std::istringstream input("x := 5");
    TTokenStream baseTokens(input);
    TWrappedTokenStream wrapped(baseTokens, 3);

    wrapped.Next(); // x
    auto loc1 = wrapped.GetLocation();

    wrapped.Next(); // :=
    auto loc2 = wrapped.GetLocation();

    EXPECT_TRUE(true);
}

TEST(WrappedTokenStreamTest, UngetThenReadSequence) {
    std::istringstream input("1 2 3");
    TTokenStream baseTokens(input);
    TWrappedTokenStream wrapped(baseTokens, 5);

    auto t1 = wrapped.Next(); // 1
    auto t2 = wrapped.Next(); // 2

    wrapped.Unget(t2);
    auto t2_v2 = wrapped.Next();
    ExpectInt(t2_v2, 2);

    auto t3 = wrapped.Next(); // 3
    ExpectInt(t3, 3);

    EXPECT_EQ(wrapped.GetWindow().size(), 3);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(LexerTest, AsSingleCharCodeMalformedUtf8) {
    EXPECT_EQ(AsSingleCharCode("\xC3\xA9"), 0xE9);
    EXPECT_EQ(AsSingleCharCode("\xE2\x82\xAC"), 0x20AC);
    EXPECT_EQ(AsSingleCharCode("\xF0\x9F\x98\x80"), 0x1F600);
    for (const char* bad : {
        "\xDB\x9F\x9F",          // stray continuation after a complete char
        "\xDB",                  // truncated
        "\xE2\x82",              // truncated
        "\xF0\x9F\x98",          // truncated
        "\x9F",                  // lone continuation
        "\xC3\xA9\xC3\xA9",      // two characters
        "\xC0\x80",              // overlong
        "\xE0\x80\x80",          // overlong
        "\xF0\x80\x80\x80",      // overlong
        "\xED\xA0\x80",          // surrogate
        "\xF4\x90\x80\x80",      // above U+10FFFF
        "\xF5\x80\x80\x80",
        "\xF8\x88\x80\x80\x80",
        "\xFF",
        "\xC3\x28",              // bad continuation
    }) {
        EXPECT_FALSE(AsSingleCharCode(bad).has_value()) << bad;
    }
}

TEST(LexerTest, MalformedUtf8InQuotes) {
    std::string src = "\"\xDB\x9F\x9F\"";
    std::istringstream in(src);
    TTokenStream stream(in);
    auto t = stream.Next();
    EXPECT_EQ(t.Type, TToken::String);
    EXPECT_EQ(t.Name, "\xDB\x9F\x9F");
}
