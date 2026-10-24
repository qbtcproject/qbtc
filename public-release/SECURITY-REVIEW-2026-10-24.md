# Security review — 24 October 2026

## Scope and conclusion

Internal source review and executable regressions against the candidate source based on commit 5da7873d376451f946c87123acc58483300caa70, plus the backup-verification correction described below. This is not an independent audit, formal proof or certification. No new critical vulnerability was demonstrated in this bounded review. Passing checks do not establish the absence of exploitable bugs.

Reviewed boundaries: consensus serialization and validation, transaction ownership/signatures, subsidy and difficulty, branch selection and UTXO undo, wallet encryption and recovery, durable storage, local authenticated control, native network framing/admission, Tor bridge/discovery, and the payment coordinator. Historical release directories were not treated as current runtime source.

## Finding fixed: backup decryption was not automatic identity verification

WALLET-ACTION.ps1 previously decrypted the selected backup and told the user to compare its address manually. The documentation described a stronger matching check than the coordinator actually performed. A valid backup for a different wallet could therefore look like a successful verification even though it would not recover the intended funds. This was an assurance/recovery failure, not a demonstrated remote key-extraction vulnerability.

Verification now compares the backup address with the active wallet's address cache, only after verifying that cache's SHA-256 binding against the current encrypted wallet file. A mismatch fails explicitly. A missing or stale binding requires unlocking the active wallet first. When no active wallet exists, the result explicitly reports decryption only, with no identity comparison. The operation never overwrites the active wallet.

The real PowerShell coordinator and native encrypted wallet were exercised with a matching backup, a different-wallet backup, a stale binding, restoration into an empty slot and existing-wallet overwrite refusal. Original wallet bytes remained unchanged. Tests use disposable wallets and Unicode/bracket paths. Passwords were not echoed. Updated documentation states the distinction.

## Coverage matrix

| Sensitive area | Mechanisms inspected | Executed evidence in this review |
| --- | --- | --- |
| Issuance and difficulty | Money-range checks, zero-reward genesis, halvings, exact expected target, valid PoW, factor-four retarget | Consensus suite; 2,016-block integration; wrong-target rejection without state mutation; shorter higher-work branch wins; candidate genesis/target identity |
| Spending authorization | Ownership from ML-DSA public key, exact signature lengths, transaction/network/input/output commitments, mature inputs, duplicate spends, no excess coinbase | Wallet suite, signed reorganization checks, relay/consensus separation; external signature vectors |
| Branches and storage | Sparse UTXO overlays, undo checks, checksummed committed journal frames, bounded sizes, durable append, atomic snapshots, writer lock | Journal/undo suite, torn-write/checksum/migration/reorganization tests; mempool persistence; node cache follows reorganization |
| Wallet and console | AES-256-GCM authenticated metadata, scrypt, random salt/nonce, protected new-file creation, hidden password input, explicit payment review | Wallet suite; signer PTY; encrypted backup restore and spend on disposable Regtest; updated coordinator identity checks |
| Local control | Loopback control, HMAC domain separated by genesis/command, constant-time tag comparison, expiry, bounded replay cache, restart key rotation | Control suite; forged control rejected; authenticated status remains available during public read quota exhaustion |
| Untrusted native peers | Frame/command/length bounds before body allocation, transfer deadlines, relay admission quotas, future-time admission window, local resource limits | Structured 20,000-input mutation campaign; admission/budget/node-policy suites; candidate transport oversized-header, public-mining and localhost injection rejection |
| Mining concurrency | Copied worker headers, disjoint nonce batches, node-thread commit, cancellation and limits | Parallel-mining suite; nonce exhaustion and forward progress |
| Panel state and payment retry | Delete-sharing state reader, bounded nonfatal telemetry write retries, per-wallet action lock, pending reservation before signing, same-bytes retry | State persistence failure/recovery fixture; ambiguous submission retained, modified transaction rejected, duplicate signing blocked, confirmation cleanup |

## Exact execution summary

Thirteen C++ suites compiled and ran with AddressSanitizer and UndefinedBehaviorSanitizer, plus one signer-console PTY suite: 14/14 returned zero. Suites: consensus, wallet, journal, control, fuzz, mempool, internet_policy, peers, parallel_mining, difficulty_launch, security_admission, request_budget, node_policy, signer_console.

The generic sanitizer suites exercise disposable Regtest and shared validation code; they are not a full rewarded-block campaign on the difficult candidate network. A separately rebuilt final-profile Linux binary passed the frozen genesis, proof, initial target, monetary constants, test-profile rejection and height-zero restart checks. No rewarded candidate blocks were mined by this review.

The actual candidate native peer passed local transport probes for header-only oversized submissions, forbidden public mining control, onion-adapter localhost injection, public quotas, forged authenticated control and safe rejection of excessive file-descriptor listeners. Temporary loopback nodes were used; these were not hostile real-Tor deployment trials.

The actual ML-DSA verifier accepted 63 valid and rejected 15 invalid empty-context Wycheproof cases distributed with checksum-pinned OpenSSL 3.5.9. Three nonempty-context cases were excluded because QBTC uses empty context. This is external-vector evidence through the same OpenSSL provider, not independent implementation equivalence testing.

Raw sanitizer suite output is available in Security-Review-2026-10-24-results.json. Linux PowerShell 7.4.6 was used for coordinator regressions. Windows PowerShell 5.1 and the native Windows executables were not executed here. Windows runtime binaries are unchanged; the wallet coordinator is updated.

## Remaining risks and missing assurance

- Tor discovery and four distinct synchronization lanes do not prove distinct operators. A participant controlling all reachable sources can isolate a node. There was no new prolonged real-Tor eclipse, Sybil or flooding campaign in this review.
- A small network is vulnerable to concentrated hashpower and deep reorganizations. Waiting for confirmations reduces some payment risk but does not create independent mining power.
- Difficulty adjusts every 2,016 blocks with a factor-four cap. Sudden hashpower changes can accelerate or stall production before adjustment; the initial reference is an assumption, not a block-time guarantee.
- Rate limits and bounded allocations reduce denial-of-service exposure but cannot guarantee availability under distributed attacks. Local block/UTXO/journal budgets can eventually stop progress; scalable operation and recovery procedures remain important.
- The candidate bundle still lacks built-in initial contacts. A fresh user needs a reachable shared candidate contact. This is an onboarding/availability limitation and must be addressed before promising unattended public bootstrap.
- Independent cryptographic/consensus review, full native Windows testing, power-loss tests on real Windows storage, long-running network trials, reproducible builds and publisher signing remain pending.
- LeakSanitizer was disabled. The static OpenSSL library was not sanitizer-instrumented. ASan/UBSan success in application code does not cover all cryptographic-library internals, timing leakage, process dumps or every thread interleaving.
- The panel does not protect against malware running as the same local user. Keeping passwords out of balance queries reduces exposure but does not make a compromised computer safe.

## Deployment scope

The correction changes backup verification and its documentation. It does not change genesis, difficulty, emission, network identity or native executable bytes. Existing wallets and chains are reused. Stop the old panel and extract the updated complete bundle into a new program folder before running it.
