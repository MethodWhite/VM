# Vex Self-Hosted Compiler

This directory contains the self-hosted Vex compiler - a complete compiler
for the Vex language written in Vex itself. This milestone proves Vex has
reached a level of expressiveness and maturity where it can compile its
own source code.

## Architecture

The compiler follows a classic multi-pass pipeline:

```
Source (.vex)
  -> Lexer   (lexer.vex)   - Tokenization
  -> Parser  (parser.vex)  - Recursive descent into AST
  -> Type Checker (type_checker.vex) - Semantic analysis
  -> Codegen (codegen.vex) - VELB bytecode emission
  -> Driver  (driver.vex)  - Pipeline orchestration
```

## Components

### ast.vex
Defines all AST node types as Vex structs:
- ASTNode base type with SourcePos tracking
- Program, FunctionDecl, VariableDecl, IfStmt, WhileStmt, etc.
- Expression nodes: BinaryOp, UnaryOp, CallExpr, literals
- Enum definitions for ASTKind, BinOpKind, UnOpKind
- ASTVisitor interface for traversal

### lexer.vex
Tokenizes Vex source into a flat token array:
- Token types: identifier, integer, float, string, keywords, operators, delimiters
- Supports all Vex keywords (fn, let, mut, if, else, etc.)
- Full operator and delimiter set
- Skips line comments (`//`) and block comments (`/* */`)
- Handles string escape sequences (`\n`, `\t`, `\"`, etc.)
- Number parsing with hex (`0x`), binary (`0b`), octal (`0o`) prefixes

### parser.vex
Recursive descent parser with full operator precedence:
- 20 keyword recognition including match/case
- Operator precedence: assignment < logical or/and < equality < comparison < term < factor < unary
- Postfix expressions: calls, field access, index access
- Match statement parsing with pattern bindings
- Error recovery via position tracking

### type_checker.vex
Semantic analysis with scope management:
- Symbol table with nested scopes (push/pop)
- Type inference for all literals and expressions
- Function declaration validation
- Struct and enum type registration
- 13 builtin primitive types pre-registered
- Error reporting with SourcePos

### codegen.vex
VELB bytecode generator:
- Produces VELB v3 compatible output
- 16 general-purpose virtual registers
- Simple register allocator
- Function prologue/epilogue (push fp / pop fp)
- All major opcodes: MOV, ADD, SUB, MUL, DIV, CMP, JMP, CALLVM, RET, PUSH, POP
- Label resolution with patch lists
- String data section support

### driver.vex
Main compiler driver:
- CLI: `vxc input.vex -o output.velb`
- Pipeline: lex -> parse -> type_check -> codegen
- Error reporting and diagnostics
- Verbose mode for debugging
- Help text with `--help`

## Building

The self-hosted compiler is compiled with the existing C++ Vex compiler:

```
vm --vex src/selfhost/driver.vex -o bin/vxc.velb
```

Then use it to compile Vex programs:

```
bin/vxc.velb input.vex -o output.velb
```

## Significance

A self-hosted compiler is the definitive proof that a language is
Turing-complete and practical. It demonstrates:

1. The language can express complex algorithms (parsing, code generation)
2. The standard library provides sufficient I/O and data structures
3. The type system is rich enough for typed AST manipulation
4. The runtime supports dynamic dispatch, recursion, and memory management

Vex joining the ranks of languages that can compile themselves marks
a major milestone in its development as a serious systems programming
language.
