#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# One pinned, checksum-verified local dependency. System OpenSSL is untouched.
python3 scripts/fetch_openssl.py
cd .deps/openssl-3.5.9
if [[ ! -f Makefile ]]; then
 ./Configure no-shared no-tests no-legacy
fi
make -j"${BUILD_JOBS:-4}" build_libs
