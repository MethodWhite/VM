# Vex Developer Tools

This directory contains developer tools for the Vex programming language:

- `vexfmt/` - Vex code formatter
- `vexlint/` - Vex linter

## vexfmt

The Vex code formatter parses Vex source code into an AST and pretty-prints it
according to configurable formatting rules. It follows the K&R brace style
(opening brace on the same line) and uses 4-space indentation (no tabs).

### Usage

```
vexfmt file.vex              Format a file in-place
vexfmt --check file.vex      Check formatting without modifying (CI mode)
vexfmt --recursive src/      Format all .vex files in a directory
vexfmt --config path file    Use custom config file
```

### Formatting Rules

| Rule | Default | Category |
|------|---------|----------|
| indent_size | 4 | Indentation |
| indent_style | spaces | Indentation |
| space_before_parenthesis | false (no space) | Spacing |
| space_around_binary | true | Spacing |
| space_after_comma | true | Spacing |
| space_after_colon | true | Spacing |
| space_after_semicolon | true | Spacing |
| max_line_width | 100 | Line breaks |
| brace_style | K&R (same line) | Line breaks |
| blank_line_between_fns | true | Line breaks |
| imports_first | true | Ordering |
| consts_before_vars | true | Ordering |
| fns_after_types | true | Ordering |
| public_before_private | true | Ordering |

### Configuration

Create a `.vexfmt.json` file in the project root:

```json
{
    "indent_size": "4",
    "max_line_width": "100",
    "space_around_binary": "true"
}
```

### Architecture

1. **Lexer** (`vexfmt.vex`): Tokenizes source into AST tokens.
2. **Parser** (`vexfmt.vex`): Builds a structural AST from tokens.
3. **Rules Engine** (`rules.vex`): Defines formatting rules as data structures.
4. **Pretty Printer** (`vexfmt.vex`): Walks the AST and emits formatted code.
5. **Config Loader** (`rules.vex`): Loads `.vexfmt.json` overrides.

## vexlint

The Vex linter analyzes Vex source code for potential bugs, style issues, and
naming convention violations. It supports auto-fix for fixable issues and CI
mode for use in automated pipelines.

### Usage

```
vexlint file.vex              Lint a file
vexlint --fix file.vex        Auto-fix fixable warnings
vexlint --ci src/             CI mode (exit code 1 on any warning/error)
vexlint --config path file    Use custom config file
vexlint -v file.vex           Verbose output
```

### Lint Rules

| Rule | Severity | Auto-fix | Description |
|------|----------|----------|-------------|
| unused_variable | Warning | No | Warns on unused let bindings |
| unused_function | Warning | No | Warns on private functions never called |
| shadowing | Warning | No | Warns on variable shadowing |
| naming_convention | Warning | Yes | snake_case for functions/vars, PascalCase for types |
| no_global_mutable | Error | No | Error on mutable global variables |
| no_unused_import | Warning | No | Warns on unused imports |
| prefer_const | Info | Yes | Suggests const over let for immutable bindings |

### Configuration

Create a `.vexlint.json` file to customize rule severities:

```json
{
    "naming_convention": "error",
    "prefer_const": "warning",
    "shadowing": "off"
}
```

### Exit Codes

| Exit Code | Meaning |
|-----------|---------|
| 0 | Lint passed (no errors/warnings in CI mode) |
| 1 | Lint found issues or file error |

### Architecture

1. **Scope Builder** (`rules.vex`): Constructs scope/symbol tables.
2. **Rule Checkers** (`rules.vex`): Individual check functions per rule.
3. **Diagnostics Engine** (`vexlint.vex`): Collects and formats diagnostics.
4. **Fix Engine** (`vexlint.vex`): Applies auto-fixes for fixable rules.
5. **Config Loader** (`vexlint.vex`): Loads `.vexlint.json` overrides.

## Development

Both tools are written in pure Vex and follow Vex conventions:

- 4-space indentation, no tabs
- K&R brace style (opening brace on same line)
- snake_case for functions and variables
- PascalCase for types
- Single blank line between function declarations

## CI Integration

For CI pipelines:

```bash
# Check formatting
vexfmt --check src/

# Lint with CI mode
vexlint --ci src/
```
