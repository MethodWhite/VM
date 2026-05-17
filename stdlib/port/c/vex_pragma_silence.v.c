// @vex-snippet: vex_pragma_silence
// @vex-requires:
// @vex-includes:
// @vex-freestanding-skip: no

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#  pragma GCC diagnostic ignored "-Wunused-variable"
#  pragma GCC diagnostic ignored "-Wunused-but-set-parameter"
#  pragma GCC diagnostic ignored "-Wunused-label"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wunused-const-variable"
#endif
