# Changelog

## [1.0.0] - 2026-06-12

### Added
- Sandbox capability checks for photonic and materia native plugins
  - Added `cap_check` function pointer to `VestaPluginAPI` struct (`include/ffi/vesta_plugin.h`)
  - Implemented `cap_check` lambda in `Loader::load_executable()` via `check_cap_at_pc` (`src/loader/loader.cpp`)
  - Added capability checks at the start of each function in `stdlib/native/photonic/vesta_photonic.c`
  - Added capability checks at the start of each function in `stdlib/native/materia/vesta_materia.c`
- CI/CD GitHub Actions workflow (`.github/workflows/ci.yml`)
  - Triggers on push and PR to main/master
  - Builds the project with CMake
  - Runs tests if available
