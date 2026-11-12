#!/bin/sh
# Signs a Windows .exe in place: Authenticode, SHA-256, with an RFC 3161 timestamp. build-windows.sh calls it for
# VaxMaps.exe, the installer and the uninstaller when one of these is set:
#   SIGN_PFX=cert.pfx [SIGN_PASS=password]                                   certificate and key in a PKCS#12 file
#   SIGN_PKCS11_MODULE=module.so SIGN_CERT=cert.pem SIGN_KEY='pkcs11:...' [SIGN_PASS=PIN]   key on a token (USB, HSM)
#   SIGN_CMD='command'   any command that signs, in place, the file given as its last argument (jsign for a cloud key...)
# SIGN_TS sets the timestamp server (default: Sectigo's); SIGN_TS= (empty) signs without one.
set -e
f="$1"
if [ -n "$SIGN_CMD" ]; then
  $SIGN_CMD "$f"
  exit
fi
TS=${SIGN_TS-http://timestamp.sectigo.com}
set -- -h sha256 -n "Vax Maps" -i https://github.com/oapocrifo/vaxmaps
[ -z "$TS" ] || set -- "$@" -ts "$TS"
[ -z "$SIGN_PASS" ] || set -- "$@" -pass "$SIGN_PASS"
if [ -n "$SIGN_PKCS11_MODULE" ]; then
  set -- "$@" -pkcs11module "$SIGN_PKCS11_MODULE" -certs "$SIGN_CERT" -key "$SIGN_KEY"
else
  set -- "$@" -pkcs12 "$SIGN_PFX"
fi
osslsigncode sign "$@" -in "$f" -out "$f.signed"
mv "$f.signed" "$f"
