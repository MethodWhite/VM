// @vex-snippet: vex_macros
// @vex-requires:
// @vex-includes:
// @vex-freestanding-skip: no

#if defined(__GNUC__) || defined(__clang__)
#  define VEX_RESTRICT __restrict__
#  define VEX_HOT      __attribute__((hot))
#  define VEX_COLD     __attribute__((cold))
#  define VEX_UNUSED   __attribute__((unused))
#  define VEX_NORETURN __attribute__((noreturn))
#  define VEX_UNREACHABLE __builtin_unreachable()
#else
#  define VEX_RESTRICT
#  define VEX_HOT
#  define VEX_COLD
#  define VEX_UNUSED
#  define VEX_NORETURN
#  define VEX_UNREACHABLE ((void)0)
#endif

#ifndef VEX_TLS
#  if defined(__GNUC__) || defined(__clang__)
#    define VEX_TLS __thread
#  elif defined(_MSC_VER)
#    define VEX_TLS __declspec(thread)
#  else
#    define VEX_TLS
#  endif
#endif
