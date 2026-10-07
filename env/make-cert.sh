#!/bin/sh
# Create a personal self-signed signing key + cert in ./keys (inside container).
# Self-signed SIS packages may use only "user-grantable" capabilities
# (e.g. NetworkServices, LocalServices, ReadUserData, WriteUserData, UserEnvironment)
# and UIDs from the 0xA0000000/0xE0000000 unprotected ranges.
set -e
mkdir -p /work/keys
cd /work/keys
[ -f selfsigned.key ] && { echo "keys/selfsigned.key already exists"; exit 0; }
# SHA-1 + RSA 2048: Symbian^3's cert parser predates SHA-2 certificates.
openssl req -x509 -newkey rsa:2048 -sha1 -nodes -days 7300 \
  -subj "/CN=${SIGNER_NAME:-SymbianApps self-signed}" \
  -keyout selfsigned.key -out selfsigned.cer
chmod 600 selfsigned.key
echo "created keys/selfsigned.key and keys/selfsigned.cer (keep the key out of git)"
