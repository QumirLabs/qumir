#pragma once

#include <string>
#include <memory>
#include <array>
#include <vector>
#include <sstream>
#include <iostream>
#include <functional>
#include <optional>
#include <ranges>
#include <span>

#include <qumir/location.h>
#include <qumir/ptr.h>
#include <qumir/parser/type_factory.h>

#include "type.h"
#include "operator.h"

namespace NQumir {
namespace NAst {

struct TExpr;
using TExprPtr = TIntrusivePtr<TExpr>;

template<typename T, typename = std::enable_if_t<std::is_base_of_v<TExpr, T>>>
using TExprPtrOf = TIntrusivePtr<T>;

// Copies one node while sharing its children, or recursively copies a parsed
// source tree with fresh scope bookkeeping. Used when a cached source module is
// composed into more than one independent compilation.
TExprPtr ShallowCloneNode(const TExprPtr& node);
TExprPtr DeepCloneExpr(const TExprPtr& node);

// Lifetime AST node forward declarations.
struct TRetainExpr;
struct TOwnLiteralExpr;
struct TMoveExpr;
struct TBorrowExpr;
struct TDestroyExpr;
struct TReplaceExpr;
struct TCleanupExitExpr;
struct TGlobalCleanupExpr;
// End lifetime AST node forward declarations.

// Non-owning view of a node as kind T, or null: checking and Cast() touch no
// reference count, so the node must stay owned elsewhere while the view is used.
template<typename T>
struct TMaybeNode {
    TMaybeNode(TExpr* node);

    template<typename U>
    TMaybeNode(const TIntrusivePtr<U>& node)
        : TMaybeNode(static_cast<TExpr*>(node.Get()))
    { }

    // A temporary owner would release the node before the view is used.
    template<typename U>
    TMaybeNode(TIntrusivePtr<U>&& node) = delete;

    T* Cast() const {
        return Node;
    }

    T* Node;
    operator bool() const { return Node != nullptr; }
};

struct IVisitor;

struct TExpr : TRefCounted<TExpr> {
    TLocation Location;
    TTypePtr Type = nullptr;
    std::optional<std::string> Origin;

    TExpr() = default;
    TExpr(TLocation loc)
        : Location(std::move(loc))
    { }
    TExpr(TLocation loc, TTypePtr type)
        : Location(std::move(loc))
        , Type(std::move(type))
    { }
    virtual ~TExpr() = default;
    virtual std::span<const TExprPtr> Children() const {
        return {};
    }
    virtual std::span<TExprPtr> MutableChildren() {
        return {};
    }
    virtual const std::string_view NodeName() const = 0;
    virtual const std::string ToString() const {
        return std::string(NodeName());
    }

    virtual void Accept(class IVisitor& visitor) = 0;
};

template<typename T>
inline TMaybeNode<T>::TMaybeNode(TExpr* node)
    : Node(node && std::string_view(T::NodeId) == node->NodeName()
        ? static_cast<T*>(node)
        : nullptr)
{ }

struct TIdentExpr : TExpr {
    static constexpr const char* NodeId = "Ident";

    std::string Name;
    explicit TIdentExpr(TLocation loc, std::string n)
        : TExpr(std::move(loc))
        , Name(std::move(n))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return "$" + Name;
    }

    void Accept(IVisitor& visitor) override;
};

struct TAssignExpr : TExpr {
    static constexpr const char* NodeId = "Assign";

    std::string Name;
    TExprPtr Value;
    TAssignExpr(TLocation loc, std::string n, const TExprPtr& v)
        : TExpr(std::move(loc))
        , Name(std::move(n))
        , Value(v)
    { }

    std::span<const TExprPtr> Children() const override {
        return { &Value, 1 };
    }

    std::span<TExprPtr> MutableChildren() override {
        return { &Value, 1 };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return std::string("$") + Name + " =";
    }

    void Accept(IVisitor& visitor) override;
};

struct TArrayAssignExpr : TExpr {
    static constexpr const char* NodeId = "ArrayAssign";

    std::string Name;
    std::vector<TExprPtr> Operands;
    TArrayAssignExpr(TLocation loc, std::string n, std::vector<TExprPtr>&& idxs, const TExprPtr& v)
        : TExpr(std::move(loc))
        , Name(std::move(n))
        , Operands(std::move(idxs))
    {
        Operands.push_back(v);
    }

    auto Indices(this auto& self) {
        return std::span(self.Operands).first(self.Operands.size() - 1);
    }

    auto& Value(this auto& self) {
        return self.Operands.back();
    }

    std::span<const TExprPtr> Children() const override {
        return Operands;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Operands;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return std::string("$") + Name + "[...] =";
    }

    void Accept(IVisitor& visitor) override;
};

struct TNumberExpr : TExpr {
    static constexpr const char* NodeId = "Number";

    union {
        int64_t IntValue;
        double FloatValue;
    };

    explicit TNumberExpr(TLocation loc, bool v, TTypeFactory& types)
        : TExpr(std::move(loc)), IntValue(v)
    {
        Type = types.Make<TBoolType>();
    }
    explicit TNumberExpr(TLocation loc, int64_t v, TTypeFactory& types)
        : TExpr(std::move(loc)), IntValue(v)
    {
        Type = types.MakeInteger();
    }
    explicit TNumberExpr(TLocation loc, double v, TTypeFactory& types)
        : TExpr(std::move(loc)), FloatValue(v)
    {
        Type = types.MakeFloat();
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const bool IsFloat() const {
        return TMaybeType<TFloatType>(Type);
    }

    const std::string ToString() const override {
        return IsFloat() ? std::to_string(FloatValue) : std::to_string(IntValue);
    }

    void Accept(IVisitor& visitor) override;
};

struct TVectorExpr : TExpr {
    static constexpr const char* NodeId = "Vector";

    std::vector<TExprPtr> Elements;
    explicit TVectorExpr(TLocation loc, std::vector<TExprPtr>&& elems)
        : TExpr(std::move(loc)), Elements(std::move(elems))
    { }

    std::span<const TExprPtr> Children() const override {
        return Elements;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Elements;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return "vec";
    }

    void Accept(IVisitor& visitor) override;
};

// c-style const char* string literal
struct TStringLiteralExpr : TExpr {
    static constexpr const char* NodeId = "StringLiteral";

    std::string Value;
    explicit TStringLiteralExpr(TLocation loc, std::string v, TTypeFactory& types)
        : TExpr(std::move(loc)), Value(std::move(v))
    {
        Type = types.Make<TStringType>();
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        // For simplicity, output escaped string:
        std::string escaped; escaped.reserve(Value.size());
        // Escape special characters
        for (size_t i = 0; i < Value.size(); ++i) {
            if (Value[i] == '\"') {
                escaped.insert(i, "\\");
            } else if (Value[i] == '\\') {
                escaped.insert(i, "\\");
            } else if (Value[i] == '\n') {
                escaped.insert(i, "\\n");
            } else if (Value[i] == '\t') {
                escaped.insert(i, "\\t");
            } else {
                escaped.push_back(Value[i]);
            }
        }
        return "\"" + escaped + "\"";
    }

    void Accept(IVisitor& visitor) override;
};

struct TUnaryExpr : TExpr {
    static constexpr const char* NodeId = "Unary";

    TOperator Operator;
    TExprPtr Operand;
    TUnaryExpr(TLocation loc, TOperator o, const TExprPtr& e)
        : TExpr(std::move(loc))
        , Operator(o)
        , Operand(e)
    { }

    std::span<const TExprPtr> Children() const override {
        return { &Operand, 1 };
    }

    std::span<TExprPtr> MutableChildren() override {
        return { &Operand, 1 };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return Operator.ToString();
    }

    void Accept(IVisitor& visitor) override;
};

struct TBinaryExpr : TExpr {
    static constexpr const char* NodeId = "Binary";

    TOperator Operator;
    std::array<TExprPtr, 2> Operands;
    TBinaryExpr(TLocation loc, TOperator o, const TExprPtr& l, const TExprPtr& r)
        : TExpr(std::move(loc))
        , Operator(o)
        , Operands{l, r}
    { }

    auto& Left(this auto& self) { return self.Operands[0]; }
    auto& Right(this auto& self) { return self.Operands[1]; }

    std::span<const TExprPtr> Children() const override {
        return Operands;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Operands;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return Operator.ToString();
    }

    void Accept(IVisitor& visitor) override;
};

struct TBlockExpr : TExpr {
    static constexpr const char* NodeId = "Block";

    std::vector<TExprPtr> Stmts;
    int32_t Scope = -1; // filled in by name resolver, 0 - root scope, -1 - unscoped

    explicit TBlockExpr(TLocation loc, std::vector<TExprPtr>&& s)
        : TExpr(std::move(loc))
        , Stmts(std::move(s))
    { }

    std::span<const TExprPtr> Children() const override {
        return Stmts;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Stmts;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TIfExpr : TExpr {
    static constexpr const char* NodeId = "IfExpr";

    std::array<TExprPtr, 3> Branches; // Cond, Then, Else (optional)

    TIfExpr(TLocation loc, const TExprPtr& c, const TExprPtr& t, const TExprPtr& e)
        : TExpr(std::move(loc))
        , Branches{c, t, e}
    { }

    auto& Cond(this auto& self) { return self.Branches[0]; }
    auto& Then(this auto& self) { return self.Branches[1]; }
    auto& Else(this auto& self) { return self.Branches[2]; }

    std::span<const TExprPtr> Children() const override {
        return std::span(Branches).first(Branches[2] ? 3 : 2);
    }

    std::span<TExprPtr> MutableChildren() override {
        return std::span(Branches).first(Branches[2] ? 3 : 2);
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TWhileStmtExpr : TExpr {
    static constexpr const char* NodeId = "WhileStmt";

    std::array<TExprPtr, 2> Bodies; // Cond, Body

    TWhileStmtExpr(TLocation loc, const TExprPtr& cond, const TExprPtr& body)
        : TExpr(std::move(loc))
        , Bodies{cond, body}
    { }

    auto& Cond(this auto& self) { return self.Bodies[0]; }
    auto& Body(this auto& self) { return self.Bodies[1]; }

    std::span<const TExprPtr> Children() const override {
        return Bodies;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Bodies;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TRepeatStmtExpr : TExpr {
    static constexpr const char* NodeId = "RepeatStmt";

    std::array<TExprPtr, 2> Bodies; // Body, Cond

    TRepeatStmtExpr(TLocation loc, const TExprPtr& body, const TExprPtr& cond)
        : TExpr(std::move(loc))
        , Bodies{body, cond}
    { }

    auto& Body(this auto& self) { return self.Bodies[0]; }
    auto& Cond(this auto& self) { return self.Bodies[1]; }

    std::span<const TExprPtr> Children() const override {
        return Bodies;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Bodies;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TForStmtExpr : TExpr {
    static constexpr const char* NodeId = "ForStmt";

    std::string VarName;

    std::array<TExprPtr, 4> Bodies; // From, To, Step, Body

    TForStmtExpr(TLocation loc, std::string varName, const TExprPtr& from, const TExprPtr& to, const TExprPtr& step, const TExprPtr& body)
        : TExpr(std::move(loc))
        , VarName(std::move(varName))
        , Bodies{from, to, step, body}
    { }

    auto& From(this auto& self) { return self.Bodies[0]; }
    auto& To(this auto& self) { return self.Bodies[1]; }
    auto& Step(this auto& self) { return self.Bodies[2]; }
    auto& Body(this auto& self) { return self.Bodies[3]; }

    std::span<const TExprPtr> Children() const override {
        return Bodies;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Bodies;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TTimesStmtExpr : TExpr {
    static constexpr const char* NodeId = "TimesStmt";

    std::array<TExprPtr, 2> Bodies; // Count, Body

    TTimesStmtExpr(TLocation loc, const TExprPtr& count, const TExprPtr& body)
        : TExpr(std::move(loc))
        , Bodies{count, body}
    { }

    auto& Count(this auto& self) { return self.Bodies[0]; }
    auto& Body(this auto& self) { return self.Bodies[1]; }

    std::span<const TExprPtr> Children() const override {
        return Bodies;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Bodies;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TBreakStmt : TExpr {
    static constexpr const char* NodeId = "Break";

    explicit TBreakStmt(TLocation loc)
        : TExpr(std::move(loc))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TContinueStmt : TExpr {
    static constexpr const char* NodeId = "Continue";

    explicit TContinueStmt(TLocation loc)
        : TExpr(std::move(loc))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TReturnExpr : TExpr {
    static constexpr const char* NodeId = "Return";

    TExprPtr Value; // optional; null for `(return)`

    TReturnExpr(TLocation loc, const TExprPtr& value)
        : TExpr(std::move(loc))
        , Value(value)
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::span<const TExprPtr> Children() const override {
        return { &Value, Value ? 1u : 0u };
    }

    std::span<TExprPtr> MutableChildren() override {
        return { &Value, Value ? 1u : 0u };
    }

    void Accept(IVisitor& visitor) override;
};

struct TVarStmt : TExpr {
    static constexpr const char* NodeId = "Var";

    std::string Name;
    static constexpr size_t BoundsOffset = 1;
    std::vector<TExprPtr> Operands; // Init (optional), then lower and upper of each array bound
    TAccess Access; // readable/mutable; parameters: арг = {true,false}, рез = {false,true}, арг рез = {true,true}

    TVarStmt(TLocation loc, std::string name, NAst::TTypePtr type, std::vector<std::pair<TExprPtr, TExprPtr>>&& bounds = {})
        : TExpr(std::move(loc), std::move(type))
        , Name(std::move(name))
    {
        Operands.reserve(BoundsOffset + 2 * bounds.size());
        Operands.emplace_back();
        for (auto& [lower, upper] : bounds) {
            Operands.push_back(std::move(lower));
            Operands.push_back(std::move(upper));
        }
    }

    // for (var name = expr) form; type inferred from Init
    auto& Init(this auto& self) { return self.Operands[0]; }

    auto Bounds(this auto& self) {
        auto* slots = self.Operands.data() + BoundsOffset;
        using TSlot = decltype((*slots));
        return std::views::iota(size_t{0}, (self.Operands.size() - BoundsOffset) / 2)
            | std::views::transform([slots](size_t i) {
                return std::pair<TSlot, TSlot>(slots[2 * i], slots[2 * i + 1]);
            });
    }

    void AddBound(const TExprPtr& lower, const TExprPtr& upper) {
        Operands.push_back(lower);
        Operands.push_back(upper);
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::span<const TExprPtr> Children() const override {
        return std::span(Operands).subspan(Operands[0] ? 0 : BoundsOffset);
    }

    std::span<TExprPtr> MutableChildren() override {
        return std::span(Operands).subspan(Operands[0] ? 0 : BoundsOffset);
    }

    const std::string ToString() const override {
        return std::string(NodeName()) + " $" + Name;
    }

    void Accept(IVisitor& visitor) override;
};

struct TVarsBlockExpr : TExpr {
    // like TBlockExpr but without scope, used during parsing
    static constexpr const char* NodeId = "VarsBlock";

    std::vector<TExprPtr> Vars; // decl or assignments

    explicit TVarsBlockExpr(TLocation loc, std::vector<TExprPtr>&& vars)
        : TExpr(std::move(loc))
        , Vars(std::move(vars))
    { }

    std::span<const TExprPtr> Children() const override {
        return Vars;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Vars;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

using TParam = TExprPtrOf<TVarStmt>;

struct TGenericParam {
    enum class EKind {
        Type,
        Value,
    };

    std::string Name;
    EKind Kind;
    TTypePtr ValueType = nullptr; // optional, for value params only
};

struct TFunDecl : TExpr {
    static constexpr const char* NodeId = "FunDecl";

    std::string Name;
    std::string OriginalName; // retained for debug info when Name becomes a synthetic symbol
    std::string MangledName;
    std::vector<TGenericParam> GenericParams;
    std::vector<TParam> Params;
    std::array<TExprPtr, 2> Operands; // Body, LastAssert (optional, executed before return)
    using TPacked = uint64_t(*)(const uint64_t* args, size_t argCount);
    TPacked Packed = nullptr; // packed thunk for built-in functions
    using TInlineFactory = std::function<TExprPtr(std::vector<TExprPtr>)>;
    std::optional<TInlineFactory> InlineFactory; // if set, call is replaced by the returned AST
    bool RequireArgsMaterialization = false; // if true, arguments must be materialized before calling, used for strings
    bool NeedsLocator = false; // external function: calls are preceded by builtin::record_locator(line, byte, column)
    NAst::TTypePtr RetType = nullptr; // ret type different from TExpr::Type which is the function value type
    int32_t Scope = -1; // Function internal scope, filled in by name resolver, -1 - unscoped
    // (operator "X") attribute: this function implements operator/cast "X"
    // (e.g. "+", "neg", "cast", "print"); registered into the resolver's
    // type-keyed op/cast dispatch tables instead of plain name lookup.
    std::optional<std::string> OperatorName;
    // (literal "X") attribute: this function is the constructor for the numeric
    // literal suffix "X" (e.g. `2i` lowers to a call of this function).
    std::optional<std::string> LiteralSuffix;
    // `used` attribute: keep this function even when no explicit reference makes
    bool Used = false;
    // `cacheable` attribute: content-stable and reusable across compilations,
    // so its compiled object may be persisted in the JIT object cache.
    bool Cacheable = false;
    // `inline` attribute: request LLVM alwaysinline on a function definition.
    bool Inline = false;
    // Set once the resolver finalized the emit symbol (overload/cacheable mangling);
    // guards against re-mangling when the mangled name is re-declared.
    bool Mangled = false;
    TFunDecl(TLocation loc, const std::string& name, std::vector<TGenericParam>&& genericParams, std::vector<TParam>&& args, const TExprPtrOf<TBlockExpr>& body, const NAst::TTypePtr& type)
        : TExpr(std::move(loc))
        , Name(std::move(name))
        , GenericParams(std::move(genericParams))
        , Params(std::move(args))
        , Operands{body, nullptr}
        , RetType(std::move(type))
    { }

    TBlockExpr* Body() const { return static_cast<TBlockExpr*>(Operands[0].Get()); }
    auto& BodyExpr(this auto& self) { return self.Operands[0]; }
    auto& LastAssert(this auto& self) { return self.Operands[1]; }

    // A mangled/link symbol marks an external function, except a cacheable one
    // which carries a MangledName (its emit symbol) yet still has a body.
    // Bodyless but NOT external: inline/intrinsic builtins (InlineFactory/Packed,
    // no MangledName) - expanded at the call site, so nothing to lower or link.
    bool IsExternal() const {
        return !MangledName.empty() && !(Cacheable && Body());
    }

    std::span<const TExprPtr> Children() const override {
        return std::span(Operands).first(Operands[1] ? 2 : 1);
    }

    std::span<TExprPtr> MutableChildren() override {
        return std::span(Operands).first(Operands[1] ? 2 : 1);
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        auto s = std::string(NodeName()) + " $" + Name;
        s += " (";
        for (size_t i = 0; i < Params.size(); ++i) {
            s += "$" + Params[i]->Name;
            if (!Type && Params[i]->Type) {
                std::ostringstream str; str << *Params[i]->Type;
                s += ": " + str.str();
            }
            if (i < Params.size() - 1) {
                s += ", ";
            }
        }
        s += ")";
        if (!Type && RetType) {
            std::ostringstream str; str << *RetType;
            s += " -> " + str.str();
        }
        return s;
    }

    void Accept(IVisitor& visitor) override;
};

struct TCallExpr : TExpr {
    static constexpr const char* NodeId = "Call";

    static constexpr size_t ArgsOffset = 1;
    std::vector<TExprPtr> Operands; // Callee, Args...
    TCallExpr(TLocation loc, const TExprPtr& c, std::vector<TExprPtr>&& a)
        : TExpr(std::move(loc))
    {
        Operands.reserve(ArgsOffset + a.size());
        Operands.push_back(c);
        Operands.insert(Operands.end(), std::make_move_iterator(a.begin()), std::make_move_iterator(a.end()));
    }

    // should evaluate to a function
    auto& Callee(this auto& self) { return self.Operands[0]; }
    auto Args(this auto& self) { return std::span(self.Operands).subspan(ArgsOffset); }

    std::span<const TExprPtr> Children() const override {
        return Operands;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Operands;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TAwaitExpr : TExpr {
    static constexpr const char* NodeId = "Await";

    TExprPtr Operand;
    explicit TAwaitExpr(TLocation loc, const TExprPtr& operand)
        : TExpr(std::move(loc))
        , Operand(operand)
    { }

    std::span<const TExprPtr> Children() const override {
        return { &Operand, 1 };
    }

    std::span<TExprPtr> MutableChildren() override {
        return { &Operand, 1 };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

// Sugared AST node for input(...) expression, which is transformed into calls to input_xxx functions
struct TInputExpr : TExpr {
    static constexpr const char* NodeId = "Input";

    std::vector<TExprPtr> Args;
    TInputExpr(TLocation loc, std::vector<TExprPtr>&& a, TTypeFactory& types)
        : TExpr(std::move(loc))
        , Args(std::move(a))
    {
        Type = types.Make<TVoidType>();
    }

    std::span<const TExprPtr> Children() const override {
        return Args;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Args;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

// Sugared AST node for output(...) expression, which is transformed into calls to output_xxx functions
struct TOutputArg {
    TExprPtr Expr;
    TExprPtr Width; // optional width expression
    TExprPtr Precision; // optional precision expression
};

struct TOutputArgRef {
    TExprPtr& Expr;
    TExprPtr& Width;
    TExprPtr& Precision;
};

struct TOutputExpr : TExpr {
    static constexpr const char* NodeId = "Output";
    static constexpr size_t ArgStride = 3;

    std::vector<TExprPtr> Operands; // Expr, Width, Precision of each argument; Width and Precision may be null
    TOutputExpr(TLocation loc, std::vector<TOutputArg>&& a, TTypeFactory& types)
        : TExpr(std::move(loc))
    {
        Type = types.Make<TVoidType>();
        Operands.reserve(ArgStride * a.size());
        for (auto& arg : a) {
            Operands.push_back(std::move(arg.Expr));
            Operands.push_back(std::move(arg.Width));
            Operands.push_back(std::move(arg.Precision));
        }
    }

    auto Args() {
        auto* slots = Operands.data();
        return std::views::iota(size_t{0}, Operands.size() / ArgStride)
            | std::views::transform([slots](size_t i) {
                return TOutputArgRef{
                    slots[ArgStride * i],
                    slots[ArgStride * i + 1],
                    slots[ArgStride * i + 2],
                };
            });
    }

    std::span<const TExprPtr> Children() const override {
        return Operands;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Operands;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TCastExpr : public TExpr {
    static constexpr const char* NodeId = "Cast";

    TExprPtr Operand;

    TCastExpr(TLocation loc, const TExprPtr& operand, const TTypePtr& target)
        : TExpr(std::move(loc))
        , Operand(operand)
    {
        Type = target;
    }

    std::span<const TExprPtr> Children() const override {
        return { &Operand, 1 };
    }

    std::span<TExprPtr> MutableChildren() override {
        return { &Operand, 1 };
    }

    const std::string ToString() const override {
        return std::string("Cast<") + (Type ? std::string(Type->TypeName()) : std::string("?")) + ">";
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TBitcastExpr : public TExpr {
    static constexpr const char* NodeId = "Bitcast";

    TExprPtr Operand;

    TBitcastExpr(TLocation loc, const TExprPtr& operand, TTypePtr target)
        : TExpr(std::move(loc))
        , Operand(operand)
    {
        Type = std::move(target);
    }

    std::span<const TExprPtr> Children() const override {
        return { &Operand, 1 };
    }

    std::span<TExprPtr> MutableChildren() override {
        return { &Operand, 1 };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

inline TExprPtr MakeCast(const TExprPtr& e, const TTypePtr& to) {
    return MakeIntrusive<TCastExpr>(e->Location, std::move(e), std::move(to));
}

struct TIndexExpr : public TExpr {
    static constexpr const char* NodeId = "Index";

    std::array<TExprPtr, 2> Operands; // Collection, Index

    TIndexExpr(TLocation loc, const TExprPtr& collection, const TExprPtr& index)
        : TExpr(std::move(loc))
        , Operands{collection, index}
    { }

    auto& Collection(this auto& self) { return self.Operands[0]; }
    auto& Index(this auto& self) { return self.Operands[1]; }

    std::span<const TExprPtr> Children() const override {
        return Operands;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Operands;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TMultiIndexExpr : public TExpr {
    static constexpr const char* NodeId = "MultiIndex";

    static constexpr size_t IndicesOffset = 1;
    std::vector<TExprPtr> Operands; // Collection, Indices...

    TMultiIndexExpr(TLocation loc, const TExprPtr& collection, std::vector<TExprPtr>&& indices)
        : TExpr(std::move(loc))
    {
        Operands.reserve(IndicesOffset + indices.size());
        Operands.push_back(collection);
        Operands.insert(Operands.end(), std::make_move_iterator(indices.begin()), std::make_move_iterator(indices.end()));
    }

    auto& Collection(this auto& self) { return self.Operands[0]; }
    auto Indices(this auto& self) { return std::span(self.Operands).subspan(IndicesOffset); }

    std::span<const TExprPtr> Children() const override {
        return Operands;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Operands;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TSliceExpr : public TExpr {
    static constexpr const char* NodeId = "Slice";

    std::array<TExprPtr, 3> Operands; // Collection, Start, End

    TSliceExpr(TLocation loc, const TExprPtr& collection, const TExprPtr& start, const TExprPtr& end)
        : TExpr(std::move(loc))
        , Operands{collection, start, end}
    { }

    auto& Collection(this auto& self) { return self.Operands[0]; }
    auto& Start(this auto& self) { return self.Operands[1]; }
    auto& End(this auto& self) { return self.Operands[2]; }

    std::span<const TExprPtr> Children() const override {
        return Operands;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Operands;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

class TUseExpr : public TExpr {
public:
    static constexpr const char* NodeId = "Use";
    std::string ModuleName;
    bool Resolved = false;

    explicit TUseExpr(TLocation loc, const std::string& moduleName)
        : TExpr(std::move(loc))
        , ModuleName(std::move(moduleName))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return "use \"" + ModuleName + "\"";
    }

    void Accept(IVisitor& visitor) override;
};

class TAssertStmt : public TExpr {
public:
    static constexpr const char* NodeId = "Assert";
    TExprPtr Expr;

    explicit TAssertStmt(TLocation loc, const TExprPtr& expr)
        : TExpr(std::move(loc))
        , Expr(expr)
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::span<const TExprPtr> Children() const override {
        return { &Expr, 1 };
    }

    std::span<TExprPtr> MutableChildren() override {
        return { &Expr, 1 };
    }

    void Accept(IVisitor& visitor) override;
};

class TTypeDeclStmt : public TExpr {
public:
    static constexpr const char* NodeId = "TypeDecl";

    std::vector<TGenericParam> GenericParams;

    explicit TTypeDeclStmt(
        TLocation loc,
        TTypePtr type,
        std::vector<TGenericParam> genericParams = {})
        : TExpr(std::move(loc), std::move(type))
        , GenericParams(std::move(genericParams))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

class TFieldAccessExpr : public TExpr {
public:
    static constexpr const char* NodeId = "FieldAccess";
    TExprPtr Object;
    std::string FieldName;
    int FieldIndex = -1;

    TFieldAccessExpr(TLocation loc, const TExprPtr& object, const std::string& fieldName)
        : TExpr(std::move(loc))
        , Object(object)
        , FieldName(std::move(fieldName))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::span<const TExprPtr> Children() const override {
        return { &Object, 1 };
    }

    std::span<TExprPtr> MutableChildren() override {
        return { &Object, 1 };
    }

    void Accept(IVisitor& visitor) override;
};

// Constructs a struct value from per-field expressions. Type must be set to the struct type.
// Used by inline factories to build struct results without requiring a TVarStmt.
class TStructConstructExpr : public TExpr {
public:
    static constexpr const char* NodeId = "StructConstruct";
    std::vector<TExprPtr> Fields;
    std::vector<std::string> FieldNames;

    TStructConstructExpr(TLocation loc, TTypePtr structType, std::vector<TExprPtr>&& fields, std::vector<std::string>&& fieldNames = {})
        : TExpr(std::move(loc))
        , Fields(std::move(fields))
        , FieldNames(std::move(fieldNames))
    {
        Type = std::move(structType);
    }

    const std::string_view NodeName() const override { return NodeId; }

    std::span<const TExprPtr> Children() const override { return Fields; }
    std::span<TExprPtr> MutableChildren() override { return Fields; }

    void Accept(IVisitor& visitor) override;
};

class TFieldAssignExpr : public TExpr {
public:
    static constexpr const char* NodeId = "FieldAssign";
    std::array<TExprPtr, 2> Operands; // Object, Value
    std::string FieldName;
    int FieldIndex = -1;

    TFieldAssignExpr(TLocation loc, const TExprPtr& object, const std::string& fieldName, const TExprPtr& value)
        : TExpr(std::move(loc))
        , Operands{object, value}
        , FieldName(std::move(fieldName))
    { }

    auto& Object(this auto& self) { return self.Operands[0]; }
    auto& Value(this auto& self) { return self.Operands[1]; }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::span<const TExprPtr> Children() const override {
        return Operands;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Operands;
    }

    void Accept(IVisitor& visitor) override;
};

// Lifetime AST nodes.
struct TRetainExpr : TExpr {
    static constexpr const char* NodeId = "Retain";

    TExprPtr Value;

    TRetainExpr(TLocation loc, const TExprPtr& value)
        : TExpr(std::move(loc), value ? value->Type : nullptr)
        , Value(value)
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::span<const TExprPtr> Children() const override { return { &Value, 1 }; }
    std::span<TExprPtr> MutableChildren() override { return { &Value, 1 }; }
    void Accept(IVisitor& visitor) override;
};

struct TOwnLiteralExpr : TExpr {
    static constexpr const char* NodeId = "OwnLiteral";

    TExprPtr Value;

    TOwnLiteralExpr(TLocation loc, const TExprPtr& value)
        : TExpr(std::move(loc), value ? value->Type : nullptr)
        , Value(value)
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::span<const TExprPtr> Children() const override { return { &Value, 1 }; }
    std::span<TExprPtr> MutableChildren() override { return { &Value, 1 }; }
    void Accept(IVisitor& visitor) override;
};

struct TMoveExpr : TExpr {
    static constexpr const char* NodeId = "Move";

    TExprPtr Value;

    TMoveExpr(TLocation loc, const TExprPtr& value)
        : TExpr(std::move(loc), value ? value->Type : nullptr)
        , Value(value)
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::span<const TExprPtr> Children() const override { return { &Value, 1 }; }
    std::span<TExprPtr> MutableChildren() override { return { &Value, 1 }; }
    void Accept(IVisitor& visitor) override;
};

struct TBorrowExpr : TExpr {
    static constexpr const char* NodeId = "Borrow";

    TExprPtr Value;

    TBorrowExpr(TLocation loc, const TExprPtr& value)
        : TExpr(std::move(loc), value ? value->Type : nullptr)
        , Value(value)
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::span<const TExprPtr> Children() const override { return { &Value, 1 }; }
    std::span<TExprPtr> MutableChildren() override { return { &Value, 1 }; }
    void Accept(IVisitor& visitor) override;
};

struct TDestroyExpr : TExpr {
    static constexpr const char* NodeId = "Destroy";

    std::array<TExprPtr, 2> Bodies; // Value, Aux

    TDestroyExpr(TLocation loc, const TExprPtr& value, TTypeFactory& types, const TExprPtr& aux = nullptr)
        : TExpr(std::move(loc), types.Make<TVoidType>())
        , Bodies{value, aux}
    { }

    auto& Value(this auto& self) { return self.Bodies[0]; }
    auto& Aux(this auto& self) { return self.Bodies[1]; }

    const std::string_view NodeName() const override { return NodeId; }

    std::span<const TExprPtr> Children() const override {
        return std::span(Bodies).first(Bodies[1] ? 2 : 1);
    }

    std::span<TExprPtr> MutableChildren() override {
        return std::span(Bodies).first(Bodies[1] ? 2 : 1);
    }

    void Accept(IVisitor& visitor) override;
};

struct TReplaceExpr : TExpr {
    static constexpr const char* NodeId = "Replace";

    std::array<TExprPtr, 2> Bodies; // Target, Value

    TReplaceExpr(TLocation loc, const TExprPtr& target, const TExprPtr& value, TTypeFactory& types)
        : TExpr(std::move(loc), types.Make<TVoidType>())
        , Bodies{target, value}
    { }

    auto& Target(this auto& self) { return self.Bodies[0]; }
    auto& Value(this auto& self) { return self.Bodies[1]; }

    const std::string_view NodeName() const override { return NodeId; }

    std::span<const TExprPtr> Children() const override {
        return Bodies;
    }

    std::span<TExprPtr> MutableChildren() override {
        return Bodies;
    }

    void Accept(IVisitor& visitor) override;
};

enum class ECleanupExitKind {
    Break,
    Continue,
    Return,
};

struct TCleanupExitExpr : TExpr {
    static constexpr const char* NodeId = "CleanupExit";

    static constexpr size_t CleanupsOffset = 1;
    ECleanupExitKind Kind;
    std::vector<TExprPtr> Operands; // Value (optional), Cleanups...

    TCleanupExitExpr(
        TLocation loc,
        ECleanupExitKind kind,
        const TExprPtr& value,
        std::vector<TExprPtr>&& cleanups,
        TTypeFactory& types)
        : TExpr(std::move(loc), types.Make<TVoidType>())
        , Kind(kind)
    {
        Operands.reserve(CleanupsOffset + cleanups.size());
        Operands.push_back(value);
        Operands.insert(Operands.end(), std::make_move_iterator(cleanups.begin()), std::make_move_iterator(cleanups.end()));
    }

    auto& Value(this auto& self) { return self.Operands[0]; }
    auto Cleanups(this auto& self) { return std::span(self.Operands).subspan(CleanupsOffset); }

    const std::string_view NodeName() const override { return NodeId; }

    std::span<const TExprPtr> Children() const override {
        return std::span(Operands).subspan(Operands[0] ? 0 : CleanupsOffset);
    }

    std::span<TExprPtr> MutableChildren() override {
        return std::span(Operands).subspan(Operands[0] ? 0 : CleanupsOffset);
    }

    void Accept(IVisitor& visitor) override;
};

struct TGlobalCleanupExpr : TExpr {
    static constexpr const char* NodeId = "GlobalCleanup";

    std::vector<TExprPtr> Cleanups;

    TGlobalCleanupExpr(TLocation loc, std::vector<TExprPtr>&& cleanups, TTypeFactory& types)
        : TExpr(std::move(loc), types.Make<TVoidType>())
        , Cleanups(std::move(cleanups))
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::span<const TExprPtr> Children() const override { return Cleanups; }
    std::span<TExprPtr> MutableChildren() override { return Cleanups; }

    void Accept(IVisitor& visitor) override;
};
// End lifetime AST nodes.

struct IVisitor {
    virtual ~IVisitor() = default;
    virtual void Visit(TIdentExpr& node) = 0;
    virtual void Visit(TAssignExpr& node) = 0;
    virtual void Visit(TArrayAssignExpr& node) = 0;
    virtual void Visit(TNumberExpr& node) = 0;
    virtual void Visit(TStringLiteralExpr& node) = 0;
    virtual void Visit(TUnaryExpr& node) = 0;
    virtual void Visit(TBinaryExpr& node) = 0;
    virtual void Visit(TBlockExpr& node) = 0;
    virtual void Visit(TIfExpr& node) = 0;
    virtual void Visit(TWhileStmtExpr& node) = 0;
    virtual void Visit(TRepeatStmtExpr& node) = 0;
    virtual void Visit(TForStmtExpr& node) = 0;
    virtual void Visit(TTimesStmtExpr& node) = 0;
    virtual void Visit(TBreakStmt& node) = 0;
    virtual void Visit(TContinueStmt& node) = 0;
    virtual void Visit(TReturnExpr& node) = 0;
    virtual void Visit(TVarStmt& node) = 0;
    virtual void Visit(TVarsBlockExpr& node) = 0;
    virtual void Visit(TFunDecl& node) = 0;
    virtual void Visit(TCallExpr& node) = 0;
    virtual void Visit(TAwaitExpr& node) = 0;
    virtual void Visit(TInputExpr& node) = 0;
    virtual void Visit(TOutputExpr& node) = 0;
    virtual void Visit(TCastExpr& node) = 0;
    virtual void Visit(TBitcastExpr& node) = 0;
    virtual void Visit(TIndexExpr& node) = 0;
    virtual void Visit(TMultiIndexExpr& node) = 0;
    virtual void Visit(TSliceExpr& node) = 0;
    virtual void Visit(TUseExpr& node) = 0;
    virtual void Visit(TAssertStmt& node) = 0;
    virtual void Visit(TTypeDeclStmt& node) = 0;
    virtual void Visit(TFieldAccessExpr& node) = 0;
    virtual void Visit(TStructConstructExpr& node) = 0;
    virtual void Visit(TFieldAssignExpr& node) = 0;
    virtual void Visit(TRetainExpr& node) = 0;
    virtual void Visit(TOwnLiteralExpr& node) = 0;
    virtual void Visit(TMoveExpr& node) = 0;
    virtual void Visit(TBorrowExpr& node) = 0;
    virtual void Visit(TDestroyExpr& node) = 0;
    virtual void Visit(TReplaceExpr& node) = 0;
    virtual void Visit(TCleanupExitExpr& node) = 0;
    virtual void Visit(TGlobalCleanupExpr& node) = 0;
    virtual void Visit(TVectorExpr& node) = 0;

    virtual void VisitOtherwise(TExpr& node) = 0;
     // Add more
};

template<typename TransformFunctor, typename FilterFunctor>
bool TransformAst(TExprPtr& result, TExprPtr node, TransformFunctor f, FilterFunctor filter) {
    if (!node) return false;
    if (!filter(node)) return false;
    bool changed = false;
    for (auto& child : node->MutableChildren()) {
        if (child) {
            changed |= TransformAst(child, child, f, filter);
        }
    }
    result = f(node);
    return changed || result != node;
}

template<typename TransformFunctor, typename FilterFunctor>
bool PreorderTransformAst(TExprPtr& result, TExprPtr node, TransformFunctor f, FilterFunctor filter) {
    if (!node) return false;
    if (!filter(node)) return false;
    bool changed = false;
    result = f(node);
    changed |= result != node;
    if (!changed) {
        // no change, recurse into children of the original node
        for (auto& child : result->MutableChildren()) {
            if (child) {
                changed |= PreorderTransformAst(child, child, f, filter);
            }
        }
    }
    return changed;
}

std::ostream& operator<<(std::ostream& os, const TExprPtr& expr);

} // namespace NAst
} // namespace NQumir
