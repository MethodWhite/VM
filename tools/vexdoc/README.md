# VexDoc - Vex Documentation Generator

Automatically generates HTML/markdown documentation from Vex source code by parsing `///` doc comments.

## Usage

```bash
# Generate markdown to stdout
vexdoc file.vex

# Generate HTML output
vexdoc --html file.vex -o docs/

# Generate docs for an entire project
vexdoc --project src/ -o docs/

# Start preview server
vexdoc --serve docs/
```

## Doc Comment Format

VexDoc parses triple-slash (`///`) documentation comments. Docs are associated with the declaration immediately following them.

```vex
/// Calculate the Fibonacci sequence up to n terms.
/// Uses an iterative approach for O(n) performance.
/// @param n Number of terms to generate
/// @return Array of Fibonacci numbers
fn fibonacci(n: i32) -> i32[] {
    // ...
}
```

### Supported Tags

| Tag | Description |
|-----|-------------|
| `@param name desc` | Documents a function parameter |
| `@return desc` | Documents the return value |
| `@example code` | Provides a usage example |
| `@see ref` | Reference to related functionality |
| `@deprecated msg` | Marks an item as deprecated |

## Architecture

```
tools/vexdoc/
  vexdoc.vex         - CLI entry point and markdown generator
  extract.vex        - Doc comment parser and extractor
  html_gen.vex       - HTML page generator
  templates/
    template.html    - HTML page template
    style.css        - Dark/light theme stylesheet
    search.js        - Client-side search functionality
  README.md          - This file
```

### Components

1. **extract.vex** - Parses Vex source files, extracts `///` doc comments, function signatures, type definitions, struct fields, and doc attributes. Associates each doc comment with its following declaration.

2. **html_gen.vex** - Takes extracted documentation data and generates complete HTML pages. Produces a navigation sidebar, type/function listings with signatures, parameter tables, and embedded CSS/JS.

3. **vexdoc.vex** - Main CLI tool that dispatches to the appropriate mode (markdown, HTML, project, server).

## Features

- Extracts `///` doc comments with markdown content
- Supports `@param`, `@return`, `@example`, `@see`, `@deprecated` tags
- Parses function signatures with parameters and return types
- Extracts struct/class type definitions and their fields
- Generates clean HTML with dark/light theme toggle
- Client-side full-text search across documentation
- Responsive design with navigation sidebar
- Module index page with summary cards

## Output

### Markdown Mode

Produces GitHub-flavored markdown to stdout with:
- Module headers and file references
- Type definitions with field tables
- Function signatures with parameter/return documentation
- Doc tags rendered as sections

### HTML Mode

Generates a static HTML site with:
- Index page listing all modules as cards
- Individual module pages with full documentation
- Navigation sidebar for quick access
- Search bar with fuzzy matching
- Dark/light theme toggle
- Responsive layout for mobile viewing
