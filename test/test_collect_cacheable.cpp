#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/ir/builder.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <sstream>
#include <string>
#include <vector>

using namespace NQumir;

namespace {

const NAst::TFunDecl* FindFun(const NAst::TExprPtr& e, const std::string& name) {
    if (!e) {
        return nullptr;
    }
    if (auto f = NAst::TMaybeNode<NAst::TFunDecl>(e); f && f.Cast()->Name == name) {
        return f.Cast().Get();
    }
    for (const auto& c : e->Children()) {
        if (auto* r = FindFun(c, name)) {
            return r;
        }
    }
    return nullptr;
}

} // namespace

TEST(CollectCacheableSymbols, ByGenericNameAndByFlag) {
    NIR::TModule m;
    int symId = 1;
    auto add = [&](std::string name, bool cacheable) {
        NIR::TFunction f{};
        f.Name = std::move(name);
        f.Cacheable = cacheable;
        f.SymId = symId++;
        f.Blocks.emplace_back(); // a definition has at least one block
        m.Functions.push_back(std::move(f));
    };
    add("__generic_foo$i64", false);  // generic instance: cacheable by name
    add("filter", false);             // query kernel: not cacheable
    add("reusable_helper", true);     // generated reusable helper: flagged
    add("hash_key_value_0", false);   // query-private helper: not cacheable

    EXPECT_EQ(
        NCodeGen::CollectCacheableSymbols(m),
        (std::vector<std::string>{"__generic_foo$i64", "reusable_helper"}));
}

const NAst::TFunDecl* ParseFun(const std::string& src, const std::string& name) {
    std::istringstream in(src);
    NAst::NCore::TTokenStream tokens(in);
    NAst::NCore::TParser parser(NQumir::NTest::TestTypes());
    static NAst::TExprPtr keepAlive;
    auto parsed = parser.Parse(tokens);
    EXPECT_TRUE(parsed) << (parsed ? "" : parsed.error().ToString());
    if (!parsed) {
        return nullptr;
    }
    keepAlive = *parsed;
    return FindFun(keepAlive, name);
}

TEST(CacheableAttr, ParsedFromCoreSource) {
    const auto* cached = ParseFun(
        "(fun cached () -> i64 (attrs cacheable) (block (return (: 0 i64))))", "cached");
    ASSERT_NE(cached, nullptr);
    EXPECT_TRUE(cached->Cacheable);
}

TEST(CacheableAttr, DefaultsFalseWithoutAttribute) {
    const auto* plain = ParseFun(
        "(fun plain () -> i64 (block (return (: 0 i64))))", "plain");
    ASSERT_NE(plain, nullptr);
    EXPECT_FALSE(plain->Cacheable);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
