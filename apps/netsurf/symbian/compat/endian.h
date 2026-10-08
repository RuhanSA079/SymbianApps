/* endian.h for P.I.P.S., which has none. GCC predefines __BYTE_ORDER__,
 * which is all NetSurf (libnsfb's plot.h) looks at. */
#ifndef NS_COMPAT_ENDIAN_H
#define NS_COMPAT_ENDIAN_H
#define __LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#define __BIG_ENDIAN    __ORDER_BIG_ENDIAN__
#define __BYTE_ORDER    __BYTE_ORDER__
#endif
