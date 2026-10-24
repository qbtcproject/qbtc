# QBTC security review — 22 October 2026

## Result and boundaries
This is an internal source review and regression exercise, not an independent audit or certification. The application remains experimental and the Site remains owner-only. Reviewed baseline: `f643699553c4d97e84e0599ca940584e95396982`, with the hardening changes accompanying this report. No operator wallet, live chain, or network identity was changed during this review.

The pass covered the shipped native consensus, UTXO, journal, storage, locking, address, wallet, local-control, P2P, peer-store, mempool and parallel-miner implementations; desktop startup, wallet action, backend ownership and Tor scripts; C# onion bridge/contact management; native launcher; packaging allowlists and website document rendering. Historical prototype directories are not supported releases. OpenSSL, Tor, compilers and Windows internals were not audited from first principles. The static OpenSSL library was not sanitizer-instrumented. No physical Windows or unrelated-Internet adversarial test was available in this execution environment.

No working private-key theft, authentication bypass, unauthorized spend or supply-inflation exploit was established by these checks. This is not evidence that such flaws are absent. Five hardening findings are recorded below. Severity is a contextual internal judgment, not a CVSS score.

## Findings and corrections

| ID | Severity | Preconditions and impact | Correction and evidence |
| --- | --- | --- | --- |
| SEC-01 | Medium | Native listeners requested `SO_REUSEADDR` on Windows. Another local process can compete for the endpoint, disrupting or impersonating its service. This does not reveal the HMAC key or forge a transaction signature. | Native Windows listeners now request `SO_EXCLUSIVEADDRUSE`; C# bridge and contact listeners explicitly enable `ExclusiveAddressUse`. Windows C++ branch cross-compiles; C# property assertions pass on Linux. Actual Windows competing-bind test remains pending. |
| SEC-02 | Medium | The Tor desktop selected a private LAN interface and exposed native P2P there as well as through Tor. LAN peers could query node state, submit public transactions and consume request capacity without using Tor. Control remained loopback/HMAC protected. | The desktop backend binds native P2P and the onion forward to `127.0.0.1`. Existing CLI opt-in LAN operation is retained. Legacy loopback address announcements are disabled in onion-adapter mode so remote onions cannot introduce arbitrary localhost ports. Disposable two-node simulated-Tor backend tests pass, including stop/cleanup and unrelated-process survival. |
| SEC-03 | Medium | Unconfirmed transaction submission admitted up to the consensus block-size budget. Validation on the node thread repeatedly constructs/hashes full signing preimages and verifies signatures; large or repeatedly varied inputs create CPU amplification without requiring a mined block. There was no small relay-size/shape preflight. | New local relay policy caps one transaction at 64 KiB, 32 inputs and 128 outputs; checks version, normal-transaction structure, public-key/signature lengths before transaction hashing or signature verification. TCP `T` rejects an oversized declared length before allocating/reading its body. The receiving node and sync import both pass through admission preflight. Tests prove early rejection and that an oversized *valid signed transaction remains valid inside a mined block*: consensus is unchanged. This bounds individual work; it does not solve sustained distributed transaction flooding or rate limiting. The conservative 64 KiB policy limits a transaction to roughly 12 full ML-DSA inputs: wallets needing more inputs must use smaller separately confirmed payments or consolidation. Automatic batching is not implemented; a larger single payment can remain consensus-valid yet be refused by relay. |
| SEC-04 | Low | Child wallet/backend scripts launched bare `powershell.exe`, and Tor extraction used bare `tar.exe`, relying on executable search rules. Exploitation needs a writable searched location or manipulated environment; code replacement in the application folder itself is already outside the protection this can provide. | Shipped panel children use absolute Windows system PowerShell paths; extraction uses the system tar path. Syntax and source-call inspection pass. Windows launch behavior remains to be observed. This is defense in depth, not protection against a compromised Windows user account. |
| SEC-05 | Low | On POSIX, the main listeners were inserted into `fd_set` without checking `FD_SETSIZE`. A process with many inherited/open descriptors could invoke an out-of-bounds `FD_SET`; accepted sockets and outbound connect already had checks. It is not a demonstrated remote-only exploit in the default bounded configuration. | Listener setup now rejects high descriptors before any `FD_SET`. A disposable child retaining more than 1,030 descriptors exits with the expected error; no live data used. |

Microsoft's documented Windows socket behavior supports SEC-01:
https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse

## Validation results

Eleven native suites passed under GCC AddressSanitizer and UndefinedBehaviorSanitizer (`-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`), with strict compiler warnings treated as errors:

| Suite | Result |
| --- | --- |
| Consensus | 13,443,477 checks |
| Wallet / ML-DSA | 2,108 checks, including encrypted recovery, signatures, double spends and signed reorganization |
| Journal / undo | 1,395 checks, including torn writes, checksum, equal-work restore and deep reorganization |
| Local control | 1,039 checks, including HMAC, replay, expiry, capacity and private secret |
| Structured mutations | 20,000 inputs; 6,269 decoded; 985 valid blocks accepted; 19,015 inputs rejected atomically |
| Mempool persistence | 582 checks |
| Internet address policy | 67 checks |
| Peer persistence / discovery | 437 checks |
| Parallel mining | Mode mapping, accepted blocks, cancellation, forward nonce progress and exhaustion |
| Difficulty / work | 2,016-block integration, factor-four adjustment, wrong target rejection, shorter higher-work branch |
| New relay admission | Malformed/oversized rejection and consensus-valid over-policy signed block |

LeakSanitizer initially failed because this executor cannot inspect `/proc/<pid>/task`. It was disabled for the completed run. **No memory-leak detection pass is claimed.** ASan/UBSan found no errors in the exercised paths; they do not establish absence of other memory, concurrency or cryptographic faults. ThreadSanitizer was not run.

Additional checks passed: disposable TCP early-rejection/high-descriptor tests and rejection of legacy localhost address injection in onion mode; 12 real native-node backend lifecycle checks with simulated Tor/SOCKS and mocked Windows process ownership; 14 real C# onion syntax, bounded-book and exclusive-listener property checks; PowerShell parser checks. The hardened peer was cross-built for Windows x64 using the existing GCC/MinGW and pinned static OpenSSL toolchain. Windows execution and competing-bind testing remain pending.

The parallel-miner test had an unrelated test-harness defect: its hard-work fixture tried to instantiate a reserved Mainnet genesis in a Regtest-only build. The fixture now constructs a detached header without activating that network; production behavior is unchanged.

Machine-readable sanitizer results: `science/security-regression-results.json`. Reproduction runner in source package: `native-source/scripts/security-checks.py`. New tests: `native-source/tests/test_security_admission.cpp` and `developer-tests/test_security_transport.py`.

## Important remaining risks before public launch

1. **Peer isolation and low total work.** A reachable status response is not proof of honest history. One native adapter selects an onion contact for sequential requests; two owned bootstrap hints and a small address book do not establish eclipse/Sybil resistance. Multiple independent outbound chain views, adversarial peer tests and independent operators/connections remain necessary. A sufficiently powerful miner can reorganize a small PoW network.
2. **Resource exhaustion is bounded, not eliminated.** No comprehensive per-peer/global verification-rate budget or robust malicious-peer scoring is implemented. Sybils can still consume relay and discovery capacity. Full signing preimages are processed for each input, so large consensus-valid blocks remain computationally expensive. Active state, fork/delta indexes and journal have finite limits (including a 1 GiB journal); reaching them can stop progress. Persistence rotation/pruning and long-running load tests are needed before general use.
3. **Distribution and dependency trust.** Downloads are unsigned. Hashes on the same Site do not protect against compromise of that Site/build pipeline. Complete toolchain reproducibility, independent builds, release signing, dependency vulnerability monitoring and an independent review remain outstanding. The Tor download is checksum-pinned at acquisition, but an already extracted local runtime is reused without a complete file-manifest revalidation. Same-user malware can modify scripts/executables; this review does not make a compromised PC trustworthy.
4. **Recovery and user authorization.** There is one encrypted signing key per wallet, no BIP39/HD recovery phrase. Backup address checks are not a full clean-machine restore-and-spend exercise. Native signing currently trusts recipient/amount supplied by the local app/request; displaying and independently confirming the final destination in the signer would strengthen defense against request/clipboard substitution. Request metadata are public payment data, not passwords. Windows ACL behavior and failure/recovery need native adversarial testing.
5. **Clock and consensus design.** Future-time admission depends on the local wall clock; median-time-past and deterministic target checks remain distinct from that local policy. The 2,016-block adjustment cannot immediately absorb arbitrary hashpower growth or enforce a hard two-day issuance ceiling. Experimental calibration uses one measured PC and a user-authorized equal-throughput estimate for the second. No official genesis/launch was activated by this audit.
6. **Coverage limits.** No independent cryptographic implementation review, exhaustive parser fuzzing, Windows sanitizer run, physical Tor failure campaign, hostile-router study, supply-chain penetration test or formal consensus proof was performed. Public addresses and transactions remain public ledger information; post-quantum signatures do not provide transaction confidentiality or blanket quantum immunity.

## Operational recommendation
Keep the Site owner-only and continue disposable test-network operation. This hardening release reuses the reset-v2 network, genesis, difficulty and data directory; it does not reset balances. Update both test nodes to evaluate the transport/policy changes. Do not describe these checks as an independent security audit or a guarantee of safety for real funds.

## Follow-up
Security-Hardening-2.md records subsequent corrections to rate limiting, duplicate/index work, authenticated local health checks, signer confirmation, bounded password input and verified runtime replacement. Read it alongside this historical baseline review.
