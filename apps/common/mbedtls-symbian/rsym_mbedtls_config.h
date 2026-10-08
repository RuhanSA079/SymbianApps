/*
 * rsym_mbedtls_config.h: mbedTLS (TLS/X.509) adjustments for Symbian^3.
 * Included after the stock mbedtls_config.h via MBEDTLS_USER_CONFIG_FILE.
 */
#ifndef RSYM_MBEDTLS_CONFIG_H
#define RSYM_MBEDTLS_CONFIG_H

/* No BSD-socket glue: apps give mbedTLS their own send/recv callbacks over
 * native Symbian sockets. */
#undef MBEDTLS_NET_C

#endif
