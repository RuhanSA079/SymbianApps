/*
 * rsym_crypto_config.h: TF-PSA-Crypto (mbedTLS 4 crypto) adjustments for
 * Symbian^3. Included after the stock crypto_config.h via
 * TF_PSA_CRYPTO_USER_CONFIG_FILE.
 */
#ifndef RSYM_CRYPTO_CONFIG_H
#define RSYM_CRYPTO_CONFIG_H

/* Entropy: our mbedtls_platform_get_entropy() (TRandom::SecureRandomL)
 * instead of the Unix/Windows sources. */
#undef MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#define MBEDTLS_PSA_DRIVER_GET_ENTROPY

/* Millisecond clock: mbedtls_ms_time() in rsym_platform.cpp. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* No persistent PSA key storage: apps keep their own secrets. */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

#endif
