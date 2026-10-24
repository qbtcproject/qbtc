# QBTC

Independent proof-of-work blockchain with SHA3-512 mining and ML-DSA-65 payment signatures. Experimental software; no independent security audit.

Website and desktop downloads: https://qbtcproject.com

## Status

Source snapshot: 1.0.0-rc.1 candidate with the 24 October 2026 backup-identity verification correction. The repository also retains a separate development test-v2 profile. Candidate blocks have already been mined by initial participants; this is not a simultaneous launch from height zero. Historical reports describe their state on their publication dates.

No wallet, chain database, private Tor identity, password, compiled executable or downloaded third-party dependency is included. Historical node-contact hostnames were removed from the export. Fresh nodes still need an initial reachable peer for their selected network.

## Monetary rules

- 21,000,000 QBTC money-range cap; maximum scheduled subsidy 20,999,949.9769 QBTC.
- Zero genesis allocation; initial block subsidy 50 QBTC.
- Halving every 210,000 heights; target mean block interval 600 seconds.
- Difficulty adjustment every 2,016 blocks, capped at a factor of four.
- Mining rewards require a 100-block height difference before spending.
- No reserved team, investor or treasury allocation.

## Repository map

- `public-release/native-source/`: C++20 consensus, wallet, peer, build scripts and tests.
- `public-release/*.ps1`, `*.cs`: desktop templates, Tor transport and wallet coordinator.
- `public-release/launcher/`: Windows launcher source and icon assets.
- `public-release/FINAL-GENESIS.json`: frozen candidate genesis/initial target.
- `public-release/SECURITY-REVIEW-2026-10-24.md`: latest internal review and remaining risks.
- `scripts/`: distribution assembly and design-document generators from the source project. Website generation scripts expect the website tree, which is not part of this source-only export.

## Build on Linux

Prerequisites: C++20 compiler, Python 3.12+, Perl, Make and the usual OpenSSL build prerequisites. System OpenSSL is not replaced. Fetching the pinned dependency requires Internet access.

```sh
cd public-release/native-source
bash scripts/bootstrap-openssl.sh
QBTC_FINAL_PROFILE=1 python3 scripts/build-mainnet.py
```

Outputs are in `build-final/`. The script checks candidate genesis identity and restart at height zero; it does not mine rewarded candidate blocks. To build the retained test-v2 profile, omit QBTC_FINAL_PROFILE; outputs go to `build-mainnet/`. For disposable Regtest, use `bash scripts/build.sh`.

CMake 3.24+ is also supported. Use the checksum-verified static OpenSSL 3.5.9 build; `-DQBTC_FINAL_PROFILE=ON` selects the candidate, `-DQBTC_MAINNET=ON` alone selects retained test-v2, and neither selects Regtest. Never reuse wallet/chain directories across profiles.

## Security regression tests

From `public-release/native-source/`:

```sh
python3 scripts/security-checks.py --openssl .deps/openssl-3.5.9 --output /tmp/qbtc-security
```

The runner compiles 13 C++ suites with ASan/UBSan and runs a signer PTY suite on disposable fixtures. The static OpenSSL dependency is not sanitizer-instrumented. LeakSanitizer is opt-in. Never run tests against a real wallet or live data directory. See the dated review for exact executed scope and limitations.

## Windows and distribution

Native Windows builds: `public-release/native-source/scripts/build-windows.ps1 -FinalCandidate`; follow the dependency prerequisites in the native README. Cross-build tooling and provenance records are also included. These are not claims of reproducible or certified builds.

Desktop source templates target the retained test profile. `scripts/package-launch-candidate.py` performs the documented profile conversion for the candidate bundle. Packaging requires separately compiled Windows binaries in `public-release/bin/` and `public-release/final-bin/`, a compiled launcher, and the companion website asset tree. This source-only repository intentionally excludes those generated binaries and website output. Use the website for the existing complete desktop download.

## Contributing

Reproducible bug reports and technical review are welcome. Read CONTRIBUTING.md and SECURITY.md. Consensus changes need dedicated discussion and compatibility analysis; do not silently change genesis, target rules or monetary constants.

## License

MIT; see LICENSE. Dependency notices and licenses are retained under public-release/licenses/.
