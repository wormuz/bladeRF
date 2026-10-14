#ifndef RFIC_CHECK_H_
#define RFIC_CHECK_H_

#include <errno.h>

/* Caller provides RFIC_ERR(thing, status) for diagnostics. */
#define CHECK_BOOL(_fn)         \
    do {                        \
        int _s = _fn;           \
        if (_s < 0) {           \
            RFIC_ERR(#_fn, _s); \
            return false;       \
        }                       \
    } while (0)

/* CHECK_BOOL is for integer APIs that return zero on success and a negative
 * status on failure. Use this for bool-returning firmware helpers instead;
 * absent an errno from the helper, report I/O failure rather than an
 * unclassified negative status that becomes generic INTERNAL on the wire. */
#define CHECK_TRUE(_expr)         \
    do {                          \
        if (!(_expr)) {           \
            RFIC_ERR(#_expr, -EIO); \
            return false;         \
        }                         \
    } while (0)

#endif
