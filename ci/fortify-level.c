/* Preprocessor-only toolchain probe, not linked into IsoTab. */
#include <features.h>
#if defined(__GLIBC_PREREQ) && defined(__has_builtin)
#if __GLIBC_PREREQ(2, 33) && __has_builtin(__builtin_dynamic_object_size)
3
#else
2
#endif
#else
2
#endif
