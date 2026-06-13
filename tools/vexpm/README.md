# vexpm - Vex Package Manager

vexpm is a package manager for the Vex programming language. It handles
dependencies, building, testing, and publishing of Vex packages.

## Installation

```bash
# Clone the VM-fork repository
git clone <repository> VM-fork
cd VM-fork

# Build vexpm
vm --vex tools/vexpm/vexpm.vex -o build/vexpm.velb

# Run vexpm
vm --run build/vexpm.velb
```

## Usage

### Initialize a new package

```bash
vexpm init
```

Creates a `vex.json` manifest file and the standard directory structure:

```
my-package/
├── vex.json          # Package manifest
├── src/              # Source code
│   └── main.vex
├── tests/            # Test files
├── build/            # Build output
└── vex.lock          # Dependency lockfile
```

### Add a dependency

```bash
vexpm add std
vexpm add http
```

Adds a dependency to `vex.json` with a default constraint of `^1.0`.

### Remove a dependency

```bash
vexpm remove http
```

Removes a dependency from `vex.json`.

### Install dependencies

```bash
vexpm install
```

Resolves the dependency tree from `vex.json`, fetches matching versions
from the registry (`https://packages.vex-lang.org`), downloads them to
`vex_modules/`, and generates a `vex.lock` lockfile.

### Build the project

```bash
vexpm build
```

Runs the build script defined in `vex.json`, or falls back to compiling
`src/main.vex` with the VM compiler.

### Run tests

```bash
vexpm test
```

Runs the test script defined in `vex.json`, or compiles and executes all
`.vex` files found in the `tests/` directory. Test exit code 0 indicates
pass, any other exit code indicates failure.

### Publish a package

```bash
vexpm publish
```

Builds the project, creates a tarball, and uploads it to the registry.

## Manifest format

```json
{
    "name": "my-package",
    "version": "0.1.0",
    "description": "A Vex package",
    "license": "VMProject",
    "dependencies": {
        "std": "^1.0",
        "http": "~0.5"
    },
    "scripts": {
        "build": "vxc src/main.vex -o build/output.velb",
        "test": "vxc tests/test.vex --run"
    }
}
```

### Dependency constraints

| Prefix | Meaning                | Example  | Matches                |
|--------|------------------------|----------|------------------------|
| `^`    | Compatible with major  | `^1.0`   | `1.x.x`                |
| `~`    | Approximately equiv    | `~1.2`   | `1.2.x`                |
| `>=`   | Greater or equal       | `>=1.5`  | `1.5+`                 |
| *none* | Exact version          | `1.2.3`  | only `1.2.3`           |
| `*`    | Any version            | `*`      | all versions           |

## Lockfile

The `vex.lock` file pins exact versions for reproducible builds:

```
std@1.2.3
http@0.5.1
```

## Architecture

```
vexpm/
├── vexpm.vex       # CLI entry point and command dispatch
├── resolver.vex    # Semantic versioning and dependency graph
├── registry.vex    # HTTP registry client
├── vex.json        # Package manifest schema
└── README.md       # This file
```

### Modules

- **vexpm** - Main namespace providing CLI commands: init, add, remove,
  install, build, test, publish.
- **vexpm.resolver** - Dependency resolver with semver parsing, constraint
  matching, dependency graph with conflict detection and cycle detection,
  and lockfile read/write.
- **vexpm.registry** - Registry client that communicates with the package
  registry at `https://packages.vex-lang.org` via HTTP. Provides search,
  publish, download, and version resolution.

## License

VMProject
