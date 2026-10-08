/*
 * rsym_platform.cpp: the platform hooks mbedTLS needs on Symbian.
 *  - entropy: the system random server (TRandom::SecureRandomL on Symbian^3;
 *    GenerateRandomBytesL on S60 3rd's 9.2, which has no TRandom)
 *  - mbedtls_ms_time(): milliseconds from the system clock
 * mbedTLS headers are C; the two prototypes are declared here by hand.
 */
#include <e32base.h>
#include <e32std.h>
#include <random.h>

typedef unsigned long rsym_uint32;      /* psa_driver_get_entropy_flags_t */
typedef long long rsym_ms_time;          /* mbedtls_ms_time_t (int64_t) */

const int KPsaSuccess = 0;               /* PSA_SUCCESS */
const int KPsaErrorNotSupported = -134;  /* PSA_ERROR_NOT_SUPPORTED */
const int KPsaErrorInsufficientEntropy = -148; /* PSA_ERROR_INSUFFICIENT_ENTROPY */

extern "C" int mbedtls_platform_get_entropy(rsym_uint32 flags,
                                            unsigned int *estimate_bits,
                                            unsigned char *output,
                                            unsigned int output_size)
{
    if (flags != 0)
        return KPsaErrorNotSupported;
    TPtr8 buf(output, 0, output_size);
    buf.SetLength(output_size);
#ifdef SYMBIAN_CRYPTOSPI
    TRAPD(err, TRandom::SecureRandomL(buf));
#else
    TRAPD(err, GenerateRandomBytesL(buf));
#endif
    if (err != KErrNone)
        return KPsaErrorInsufficientEntropy;
    *estimate_bits = output_size * 8;
    return KPsaSuccess;
}

extern "C" rsym_ms_time mbedtls_ms_time(void)
{
    TTime now;
    now.UniversalTime();
    return now.Int64() / 1000;
}
