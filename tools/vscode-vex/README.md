# Vex Language Support for VS Code

Provides syntax highlighting, snippets, LSP client integration, and build/run commands for the Vex programming language.

## Features

### Syntax Highlighting
Full TextMate grammar for `.vex` files covering keywords, types, operators, comments, strings with interpolation, numbers, and annotations.

### Code Snippets
- `fn` - Function declaration
- `for` - For loop
- `while` - While loop
- `if` / `ifelse` - Conditionals
- `match` - Match expression
- `struct` - Struct definition
- `enum` - Enum definition
- `import` - Import statement
- `main` - Main function
- `println` - Print line
- `extern` - Extern FFI declaration

### LSP Client
Connects to the `vex-lsp` language server on startup for diagnostics, completions, and more.

### Commands
| Command | Description |
|---|---|
| `Vex: Compile and Run` | Compiles and runs the current file |
| `Vex: Compile` | Compiles the current file only |
| `Vex: Run Tests` | Runs the project test suite |

A status bar indicator shows compilation status.

## Requirements
- Vex compiler (`vex`) must be installed and available in `PATH`
- Vex LSP server (`vex-lsp`) must be installed and available in `PATH`

## Extension Settings
This extension contributes no additional settings.

## Building
```bash
npm install
npm run compile
```

## Release Notes

### 0.1.0
Initial release: syntax highlighting, snippets, LSP client, and run/build/test commands.
