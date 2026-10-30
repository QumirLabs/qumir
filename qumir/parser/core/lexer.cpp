/*
Core language lexical and surface syntax notes.

Delimiters:
  ( )  expression/statement forms and nested item lists
  < >  composite type forms
  :    type annotation separator

AST node forms:
  Form names describe the core language surface syntax. They intentionally do
  not have to match NAst node NodeId values one-to-one.

  Literals:
      integer, float, boolean literal -> Number
      "string"                        -> StringLiteral
      'c'                             -> Number with Char type

  name // token with identifier type
      TIdentExpr, NodeId = "Ident".

  (= name value)
      TAssignExpr, NodeId = "Assign".

  (= name [index1 ... indexN] value)
      TArrayAssignExpr, NodeId = "ArrayAssign".

  (+ operand) // 1 operand after operator
      TUnaryExpr, NodeId = "Unary".

  (+ left right)
      TBinaryExpr, NodeId = "Binary".

  (block stmt1 stmt2 ... stmtN)
      TBlockExpr, NodeId = "Block", introduces a nested lexical scope.

  (pragma group value1 value2 ...)
      Virtual node - not represented in the AST. Pragmas are collected into
      TParser::Pragmas and applied by the caller after Parse() returns.
      Pragmas are only valid at the start of the outermost (first) block,
      before any non-pragma statements:
          (block (pragma language overloads) stmt1 stmt2 ...)
      A pragma after a real statement, or in a nested block, is an error.

  (cond cond then else)
      TIfExpr, NodeId = "IfExpr".

  (if cond then else)
      TIfExpr, NodeId = "IfExpr".

  (while cond body)
      TWhileStmtExpr, NodeId = "WhileStmt".

  (repeat body cond)
      TRepeatStmtExpr, NodeId = "RepeatStmt".

  (for name from to step body)
      TForStmtExpr, NodeId = "ForStmt"; step is nil when omitted.

  (times count body)
      TTimesStmtExpr, NodeId = "TimesStmt".

  break // treat as a keyword
      TBreakStmt, NodeId = "Break".

  continue // treat as a keyword
      TContinueStmt, NodeId = "Continue".

  (var name type)
  (var name type [bound_from1 bound_to1] ... [bound_fromN bound_toN])
      TVarStmt, NodeId = "Var".

  (vars var1 var2 ... varN)
      TVarsBlockExpr, NodeId = "VarsBlock".

  (fun name return_type (param1 ... paramN) (attr1 ... attrM) body)
      TFunDecl, NodeId = "FunDecl".

  (call callee arg1 arg2 ... argN)
      TCallExpr, NodeId = "Call".

  (await operand)
      TAwaitExpr, NodeId = "Await".

  (input arg1 arg2 ... argN)
      TInputExpr, NodeId = "Input".

  (output (expr1 width1 precision1) ... (exprN widthN precisionN))
      TOutputExpr, NodeId = "Output".

  (cast operand type)
      TCastExpr, NodeId = "Cast".

  (bitcast operand type)
      TBitcastExpr, NodeId = "Bitcast".

  (retain value)
  (borrow value)
  (move value)
  (own-literal value)
  (destroy value [aux])
  (replace target value)
  (cleanup-exit (return [value]) cleanup1 ... cleanupN)
  (cleanup-exit (break) cleanup1 ... cleanupN)
  (cleanup-exit (continue) cleanup1 ... cleanupN)
  (cleanup-global cleanup1 ... cleanupN)
      Internal lifetime forms emitted after semantic analysis. They are parsed
      so the printed final AST can pass through the CoreExec round trip.

  (index collection index)
      TIndexExpr, NodeId = "Index".

  (index collection [index1 index2 ... indexN])
      TMultiIndexExpr, NodeId = "MultiIndex".

  (slice collection [start end])
      TSliceExpr, NodeId = "Slice".

  (use module_name)
      TUseExpr, NodeId = "Use".

  (assert expr)
      TAssertStmt, NodeId = "Assert".

  (field field_name object)
      TFieldAccessExpr, NodeId = "FieldAccess".

  (struct ((name1 field1) (name2 field2) ... (nameN fieldN)))
      TStructConstructExpr, NodeId = "StructConstruct".

  (field_assign object field_name value)
      TFieldAssignExpr, NodeId = "FieldAssign".

  (: ast_node type)
      Type annotation for any AST node; sets TExpr::Type.

Types:
  Primitive:
      i8/i16/i32/i64/u8/u16/u32/u64
                -> TIntegerType, TypeId = "Int"
      f32/f64   -> TFloatType, TypeId = "Float"
      bool      -> TBoolType, TypeId = "Bool"
      string    -> TStringType, TypeId = "String"
      char      -> TSymbolType, TypeId = "Char"
      void      -> TVoidType, TypeId = "Void"

  Composite:
      <fun return_type (param_type1 ... param_typeN) (attr1 ... attrM) body>
          TFunctionType, TypeId = "Fun".

      <array element_type arity>
          TArrayType, TypeId = "Array".

      <future result_type>
          TFutureType, TypeId = "Future".

      <ptr pointee_type>
          TPointerType, TypeId = "Ptr".

      <ref referenced_type>
          TReferenceType, TypeId = "Ref".

      <named name underlying_type>
          TNamedType, TypeId = "Named".

      <struct (field_name1 field_type1) ... (field_nameN field_typeN)>
          TStructType, TypeId = "Struct".

  Type examples:
      i64
      u32
      f64
      bool
      string
      char
      void
      <fun bool (i64 f64) () body>
      <array i64 1>
      <future void>
      <array <ref f64> 2>
      <ptr <struct (x f64) (y f64)>>
      <ref string>
      <named color i64>
      <struct (x i64) (values <array f64 1>)>

  The type syntax is schema-like: composite types are enclosed in < >, while
  nested lists inside a type, such as function parameter lists and struct
  fields, are enclosed in ( ).

Literals:
  Integer:
      123
      0
      42

  Float:
      1.23
      0.0
      .5
      3e-4

  Boolean:
      #t
      #f

  String:
      "hello"
      "world"

  Character:
      'a'
      '\n'

Identifiers:
  Simple identifiers start with a letter and may contain letters, digits,
  and underscores:
      foo
      bar
      my_var

  Bar identifiers may contain spaces:
      |foo bar|

Escapes:
  Strings and characters support common escapes:
      \n
      \t
      \\
      \"
      \'
*/

#include "lexer.h"

#include <qumir/parser/operator.h>

#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQumir {
namespace NAst {
namespace NCore {

namespace {

static constexpr auto Eof = std::istream::traits_type::eof();

static const std::unordered_set<int> Operators = {
    '(',
    ')',
    '<',
    '>',
    ':',
    '[',
    ']',
};

bool IsSymbolicIdentChar(char ch) {
    switch (ch) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '=':
        case '!':
        case '&':
        case '^':
        case '~':
            return true;
        default:
            return false;
    }
}

bool IsIdentStart(char ch) {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalpha(uch) || ch == '_' || ch == '$' || uch >= 0x80;
}

bool IsIdentContinue(char ch) {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_' || ch == '$' || ch == ':' || uch >= 0x80;
}

} // namespace

TTokenStream::TTokenStream(std::istream& in)
    : ITokenStream(in)
{ }

void TTokenStream::Read() {
    auto emitOperator = [&](TOperator op, const std::string& rawValue, TLocation location) {
        AfterOpenParen_ = (rawValue == "(");
        Tokens.emplace_back(TToken {
            .Value = {.i64 = (int64_t)op},
            .RawValue = rawValue,
            .Type = TToken::Operator,
            .Location = location,
        });
    };

    auto emitIdentifier = [&](const std::string& name, TLocation location) {
        AfterOpenParen_ = false;
        Tokens.emplace_back(TToken {
            .Name = name,
            .RawValue = name,
            .Type = TToken::Identifier,
            .Location = location,
        });
    };

    while (Tokens.empty() && In.peek() != Eof) {
        auto next = In.peek();
        if (std::isspace(next)) {
            Take();
            continue;
        }
        if (next == ';') {
            while (In.peek() != Eof && In.peek() != '\n') {
                Take();
            }
            continue;
        }

        TLocation tokenLocation = CurrentLocation;

        if ((next == '<' || next == '>') && AfterOpenParen_ && [&]() {
            // TODO: rewrite without look-ahead!
            In.get();
            auto second = In.peek();
            In.unget(next);
            return second == '=' || second == next;
        }()) {
            // Two-char operator (>>, >=, <<, <=) only valid as operator head after '('
            std::string name;
            name += Take();
            name += Take();
            emitIdentifier(name, tokenLocation);
        } else if (Operators.contains(next)) {
            auto ch = Take();
            emitOperator(TOperator((uint64_t)ch), std::string(1, ch), tokenLocation);
        } else if (std::isdigit(next) || next == '.' || (next == '-' && [&]() {
            In.get();
            auto second = In.peek();
            In.unget(next);
            return std::isdigit(second) || second == '.';
        }())) {
            ReadNumber(tokenLocation);
        } else if (next == '|') {
            Take();
            if (In.peek() == '|') {
                Take();
                emitIdentifier("||", tokenLocation);
            } else if (In.peek() == Eof || std::isspace(In.peek()) || In.peek() == ')') {
                emitIdentifier("|", tokenLocation);
            } else {
                std::string name;
                while (In.peek() != Eof && In.peek() != '|') {
                    name += Take();
                }
                if (In.peek() != '|') {
                    throw std::runtime_error("unterminated bar identifier at " + tokenLocation.ToString());
                }
                Take();
                emitIdentifier(name, tokenLocation);
            }
        } else if (next == '-' && [&]() {
            In.get();
            auto second = In.peek();
            In.unget(next);
            return second == '>';
        }()) {
            Take(); Take();
            emitIdentifier("->", tokenLocation);
        } else if (IsSymbolicIdentChar(next)) {
            std::string name;
            do {
                name += Take();
            } while (In.peek() != Eof && IsSymbolicIdentChar(static_cast<char>(In.peek())));
            emitIdentifier(name, tokenLocation);
        } else if (next == '#') {
            Take();
            char value = Take();
            if (value != 't' && value != 'f') {
                throw std::runtime_error("expected #t or #f at " + tokenLocation.ToString());
            }
            Tokens.emplace_back(TToken {
                .Value = {.i64 = value == 't' ? 1 : 0},
                .RawValue = std::string("#") + value,
                .Type = TToken::Boolean,
                .Location = tokenLocation,
            });
        } else if (next == '"') {
            Take();
            auto [value, rawValue] = ReadQuoted('"');
            Tokens.emplace_back(TToken {
                .Name = value,
                .RawValue = rawValue,
                .Type = TToken::String,
                .Location = tokenLocation,
            });
        } else if (next == '\'') {
            Take();
            auto [value, rawValue] = ReadQuoted('\'');
            auto chCode = AsSingleCharCode(value);
            if (!chCode) {
                throw std::runtime_error("character literal must contain exactly one character at " + tokenLocation.ToString());
            }
            Tokens.emplace_back(TToken {
                .Value = {.i64 = *chCode},
                .RawValue = rawValue,
                .Type = TToken::Char,
                .Location = tokenLocation,
            });
        } else if (IsIdentStart(next)) {
            std::string name;
            do {
                name += Take();
            } while (In.peek() != Eof && IsIdentContinue(static_cast<char>(In.peek())));
            emitIdentifier(name, tokenLocation);
        } else {
            throw std::runtime_error("unexpected character '" + std::string(1, next) + "' at " + tokenLocation.ToString());
        }
    }
}

} // namespace NCore
} // namespace NAst
} // namespace NQumir
