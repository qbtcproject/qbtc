# QBTC native source

This is private test-preview source, not the official network. The internal
`QBTC_MAINNET` switch selects the existing **test-v2** identity. Do not interpret
the profile label as an official launch or mix Regtest and test-v2 data.
`../FINAL-LAUNCH-PLAN.json` reserves future parameters but activates nothing.

## Inputs and builds

Use the pinned OpenSSL 3.5.9 source/hash in DEPENDENCIES.lock. The bundled
scripts/fetch_openssl.py and scripts/bootstrap-openssl.sh fetch/prepare that input.
Do not replace it with an unreviewed dependency version. A C++20 compiler,
Python 3 and the native OpenSSL build prerequisites are required.

On Linux, scripts/build.sh builds the Regtest tools. With CMake 3.24 or newer,
configure using OPENSSL_ROOT_DIR pointing to the verified static OpenSSL build.
QBTC_MAINNET=OFF selects disposable Regtest; QBTC_MAINNET=ON selects the existing
experimental test-v2 profile. Build and run ctest for the selected configuration.

On Windows, build the pinned vendor library as libcrypto.lib under
.deps/openssl-3.5.9 using its own INSTALL.md instructions. The project expects
Visual Studio C++ and CMake. scripts/build-windows.ps1 builds Regtest by default;
-TestNetwork selects test-v2. Native Windows execution of the newest release
changes is pending; these instructions are not a reproducible-build certification.
Cross-build tooling and exact dependency records are included for inspection.

## Security regression runner

On Linux, run scripts/security-checks.py with --openssl pointing to the verified
OpenSSL source/build folder and --output pointing to a disposable output folder.
It compiles the native suites with ASan/UBSan and runs the signer PTY tests.
LeakSanitizer is opt-in (--leak-check), because some executors cannot inspect
/proc task metadata. The static OpenSSL library itself is not instrumented by
this runner. Never run fixtures against a real wallet or live chain directory.

See ../SECURITY-REVIEW-2026-10-22.md, ../SECURITY-HARDENING-2.md and the release
readiness report for test scope, remaining risks and native Windows limitations.

## Frozen launch candidate
QBTC_MAINNET builds the retained experimental test-v2 identity by default.
QBTC_FINAL_PROFILE additionally selects the separate frozen candidate. With
CMake, use -DQBTC_FINAL_PROFILE=ON; with scripts/build-mainnet.py set environment
QBTC_FINAL_PROFILE=1. For Windows cross-builds also set QBTC_WINDOWS=1 and the
existing toolchain/OpenSSL paths. Each profile uses a separate build directory.
The script builds and checks height-zero identity only; it never mines rewarded
mainnet blocks. Do not copy wallets, chain files or peer state between profiles.
Windows native entrypoints require -municode with MinGW; the provided builders
and CMake targets apply it to the three CLI programs, not to test executables.
