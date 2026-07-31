/*
 * mpconfigport.h -- MicroPython configuration for the Active Bezel runtime.
 *
 * Sized for what a bezel actually does: read live memory, compute, and draw.
 * That means real floats (positions and time deltas are fractional), string
 * formatting (every dashboard prints numbers), and enough of the object
 * model that a script reads like Python instead of like a calculator.
 *
 * What stays OFF is as deliberate: no filesystem, no os/io, no sockets, no
 * threads. A bezel has no filesystem by design -- the same reason the Lua
 * runtime drops io/os/package -- and every one of those subsystems would
 * drag WASI imports into a wasm that must import only ab_host.
 */
#include <port/mpconfigport_common.h>

/* CORE_FEATURES gives the object model a script needs (classes, closures,
 * exceptions, slicing) without the EXTRA_FEATURES weight we cannot use. */
#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

#define MICROPY_ENABLE_COMPILER             (1)   /* scripts arrive as source */
#define MICROPY_ENABLE_GC                   (1)

/* wasm has no architecture-specific register capture for the GC's stack
 * scan; the setjmp fallback spills registers to the stack, which is exactly
 * what the collector needs to find roots. */
#define MICROPY_GCREGS_SETJMP               (1)
#define MICROPY_PY_GC                       (1)

/* Colours are 0xRRGGBBAA literals -- 0xff0000ff does not fit MicroPython's
 * 31-bit small int, and without arbitrary-precision ints the LEXER rejects
 * the literal outright ("long int not supported in this build"). Every
 * bezel writes colours that way, so this is not optional. */
#define MICROPY_LONGINT_IMPL                (MICROPY_LONGINT_IMPL_MPZ)

/* Floats: bezel coordinates are a 1920x1080 logical grid with fractional
 * scaling, and delta_ms is fractional by definition. Single precision is
 * plenty and half the size of double. */
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_PY_BUILTINS_FLOAT           (1)
#define MICROPY_PY_MATH                     (1)

/* Formatting and the string ops a dashboard leans on. */
#define MICROPY_PY_BUILTINS_STR_CENTER      (1)
#define MICROPY_PY_BUILTINS_STR_PARTITION   (1)
#define MICROPY_PY_BUILTINS_STR_SPLITLINES  (1)
#define MICROPY_PY_BUILTINS_BYTEARRAY       (1)
#define MICROPY_PY_BUILTINS_MEMORYVIEW      (1)
#define MICROPY_PY_BUILTINS_ENUMERATE       (1)
#define MICROPY_PY_BUILTINS_REVERSED        (1)
#define MICROPY_PY_BUILTINS_MIN_MAX         (1)
#define MICROPY_PY_BUILTINS_ROUND_INT       (1)
#define MICROPY_PY_ARRAY                    (1)
#define MICROPY_PY_COLLECTIONS              (1)

/* Error text: a bezel's error panel is only useful if the message says what
 * went wrong and where. */
#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_CPYTHON_COMPAT              (1)

/* CORE_FEATURES turns on `sys`, which wants a platform string. Name the
 * host honestly: a script can branch on it. */
#define MICROPY_PY_SYS_PLATFORM             "active-bezel"

/* No filesystem, no imports from disk: main.py comes from the package as a
 * string. MICROPY_VFS off is what keeps the WASI imports away. */
#define MICROPY_VFS                         (0)
#define MICROPY_READER_VFS                  (0)
#define MICROPY_PY_IO                       (0)
#define MICROPY_PY_SYS_STDFILES             (0)

/* The `ab` module is registered from C at boot (see runtime.c). */
extern const struct _mp_obj_module_t ab_module;
#define MICROPY_PORT_BUILTIN_MODULES \
    { MP_ROM_QSTR(MP_QSTR_ab), MP_ROM_PTR(&ab_module) },
