# vexbuild - Vex Build System

A build system for Vex projects supporting profiles and cross-compilation.

## Usage

```
vexbuild build --release      Build with release profile (optimizations on)
vexbuild build --debug        Build with debug profile (symbols, no opt)
vexbuild clean                Remove build artifacts
vexbuild run                  Build and run
vexbuild bench                Build and run benchmarks
vexbuild target wasm          Cross-compile to WebAssembly
vexbuild target native        Compile to native (via AOT)
vexbuild target arm64         Cross-compile to ARM64
```

## Profiles

| Profile  | opt_level | debug_info | strip | lto  | optimize_for |
|----------|-----------|------------|-------|------|--------------|
| release  | 3         | false      | true  | true | speed        |
| debug    | 0         | true       | false | false|              |
| size     | 2         | false      | true  | true | size         |

Custom profiles can be defined in `vex.json`:

```json
{
  "source": "src/main.vex",
  "output": "myapp",
  "build_dir": "target",
  "profiles": [
    {
      "name": "custom",
      "opt_level": 2,
      "debug_info": true,
      "strip": false,
      "lto": true,
      "optimize_for": "speed"
    }
  ]
}
```

## Cross-compilation Targets

| Target  | Arch    | Vendor  | System | ABI  |
|---------|---------|---------|--------|------|
| native  | x86_64  | pc      | linux  | gnu  |
| wasm    | wasm32  | unknown | none   |      |
| arm64   | aarch64 | unknown | linux  | gnu  |

Full target triples (e.g. `x86_64-pc-linux-gnu`) are also supported.

## Architecture

```
tools/vexbuild/
├── vexbuild.vex    -- Main build system (CLI, build logic)
├── profile.vex     -- Profile definitions (release, debug, size)
├── cross.vex       -- Cross-compilation target definitions
└── README.md       -- This file
```
