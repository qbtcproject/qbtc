# QBTC release readiness — frozen candidate 1.0.0-rc.1

QBTC-Windows.zip contains the separate frozen launch candidate. QBTC-Test-Windows.zip
continues the existing 0.9.0-preview.5 test chain. Both use QBTC/QBTC.exe plus app/.
The website remains owner-only. Packaging is complete; operational public launch
has not been verified and no test balances are imported.

## Implemented

The named fuckgreed genesis proof, chosen combined 2.026 MH/s reference, 600-second
spacing target and distinct candidate identity are frozen in FINAL-GENESIS.json.
Genesis and the initial epoch use the exact target; the first retarget is at 2016.
Genesis allocates nothing; no rewarded candidate blocks were mined in preparation.
Current test genesis and difficulty are preserved in the separate test build.

Four concurrent onion synchronization lanes, randomized sticky selection,
rolling introduction budgets, bounded incoming discovery, wallet/address binding,
backup verification/restoration, high-fee warnings, Unicode native entrypoints,
LiteralPath handling, one-panel locking and reduced status polling are implemented.
One unresolved payment is reserved until local active-chain confirmation; retries
resubmit identical bytes and confirmed transaction files are then cleaned.
See Network-Wallet-Review.md for each finding and the remaining limits.

## Validation

Thirteen native ASan/UBSan suites plus signer/recovery PTY regression pass on
Linux. Real C# with simulated Tor passes 26 discovery checks and four-lane/quota
fixtures. Backend lifecycle passes 12 checks from a bracket-containing folder.
Both native network profiles pass height-zero isolation/identity checks. Python
independently reproduces the named SHA3-512 genesis proof and target arithmetic.
Windows binaries cross-compile; archive hashes, CRC, runtime parity and website
links are checked. Windows execution remains pending. Static OpenSSL is not
sanitizer-instrumented and LeakSanitizer remains disabled.

## Before a public operational launch

- Native Windows 5.1/Unicode/Tor, restore and suspend/resume verification.
- First-contact setup with unrelated participants and matured candidate spending.
- Sustained hostile-peer/DoS/Sybil testing and independent security review.
- Reproducible compiler builds and publisher signing.

The included candidate has no test seed addresses. The first node must share its
new candidate onion address. Four distinct services can still share an operator;
an attacker that controls all reachable history sources can still isolate a node.
