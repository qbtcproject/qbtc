# Network, wallet and launch-candidate review

Current profiles: existing test-v2 (0.9.0-preview.5) and separate frozen candidate
(1.0.0-rc.1). Preparing the candidate does not deploy nodes to the user PCs or
prove an operational public network. Existing test data is preserved.

| Item | Assessment and resulting behavior |
| --- | --- |
| 1. Single source / eclipse | Confirmed single transport bottleneck. Four dedicated native synchronization lanes now run concurrently and remain assigned to distinct reachable onion services. Pinned seeds and long-lived contacts influence replacements. Full block/transaction validation remains mandatory. Status is only reachability: it cannot authenticate honesty, and a valid isolated fork is still possible. |
| 2. Clock-based herding | Confirmed. Removed global-clock selection. Per-process cryptographic random ranking and sticky assignments spread initial choices while avoiding constant healthy-peer churn. |
| 3. LAN exposure | Already corrected in earlier preview: native P2P binds 127.0.0.1, as do all adapter lanes and authenticated control. No firewall disabling is introduced. |
| 4. DHCP changes | The reported LAN-binding dependency is obsolete. Loopback is unaffected by DHCP or Wi-Fi address changes. Tor handles transport reconnection; backend also checks authenticated node responsiveness every ten seconds and stops with an error after repeated failure. Real suspend/resume remains a Windows test. |
| 5. Discovery lifetime quotas | Confirmed. Introduction budgets expire after one hour, increment only after successful insertion and have bounded source count. At most four retained candidates per introducer limit concentration. New persisted store retains provenance; old format remains readable. A full book of healthy contacts intentionally still has finite capacity. |
| 6. Incoming exhaustion | Partially mitigated: eight handlers, a three-second total read deadline, bounded backlog and eight admissions per second. Outgoing discovery is independent. An anonymous sustained flood can still deny incoming announcements; this is not complete DoS protection. |
| 7. Stale mining address | Corrected: encrypted-wallet SHA-256 is bound to the cached public address after native unlock/create. Mining verifies the binding before starting and checks it periodically. Changed/missing wallets stop mining. Unlock once after upgrading; ordinary balance/eye operations remain password-free. |
| 8. Restore/verify backup | Added Verify backup and Restore backup. Restore uses native decrypt/re-encrypt and protected new-file creation, only with no active wallet. Existing wallets are never overwritten. Unlock verifies the restored address before mining. |
| 9. Concurrent pending payments | Confirmed limitation of chain-only selection. Conservative resolution: one unresolved payment at a time. Retry/next Send checks the exact transaction in the local active chain before releasing its reservation. This avoids signing another payment against the same pending funds; advanced mempool-aware selection remains future wallet work. |
| 10. Excessive fees | Added a separate warning when fee exceeds payment amount or 1 QBTC, in addition to exact native confirmation. This is wallet policy, not a consensus fee cap. |
| 11. Bracket paths | Replaced affected filesystem calls with LiteralPath, including Add-Type. Backend lifecycle fixture runs from a folder containing spaces and square brackets. |
| 12. Unicode Windows paths | Added wide Windows executable entrypoints, explicit UTF-8 argument conversion and UTF-8-to-wide filesystem paths throughout native chain, control, peer, wallet and storage access. Both profiles cross-build with strict warnings. Actual Windows non-Latin usernames still require native verification. |
| 13. Inaccessible process metadata | Empty CommandLine now follows the existing clear unknown-program/port error instead of invoking a method on null. Unknown processes are never stopped. |
| 14. Duplicate panels | START holds an exclusive per-data-directory panel lock for its lifetime, with an explicit already-open error. Different profiles still share ports and must not run at the same time. |
| 15. Polling overhead | Authenticated local status now polls every three seconds online and ten seconds offline, retaining one in-flight process. Backend health has its own ten-second interval. A persistent native IPC client remains future work. |
| 16. Leftovers | A confirmed pending .tx is removed only after its exact hash/transaction ID is checked in the local active chain. Unresolved and untracked legacy transactions are not blindly deleted. Candidate Tor data uses tor/; the test package retains tor-trial/ to preserve existing identity/state. |

## Frozen candidate consensus

The message QBTC/genesis/1:fuckgreed is SHA3-512 committed in the genesis root.
This is an explicit frozen non-transaction origin commitment. Genesis has zero
transactions, no spendable outputs and no reward. Subsequent roots are normal
transaction Merkle roots. Genesis nonce 635623644 satisfies the same initial
PoW target used from height one until the first retarget at height 2016.
The candidate network ID is 0x51424d33, separate from test-v2 0x51424d32.
The reference is a chosen combined 2,026,000 H/s, not a new hardware measurement.
Expected attempts are 1,215,600,000 and expected spacing is 600 seconds at that
power. These are statistical expectations. Retarget is every 2016 blocks with
a fourfold bound. Monetary constants are unchanged: 50 QBTC initial reward,
210000-block halvings, 21 million cap, zero allocation.

The genesis timestamp is fixed at 2026-10-22 00:00 UTC. The first difficulty
period uses that origin timestamp, so a materially delayed launch affects the
first adjustment. No rewarded candidate blocks were mined while preparing this
release; only the zero-reward genesis proof was searched.

## Bootstrap and data separation

QBTC-Windows.zip is the candidate; QBTC-Test-Windows.zip continues existing tests.
The candidate uses %LOCALAPPDATA%\QBTC-mainnet-2026 and carries no test-network
seeds, wallets, chain history or onion identity. The first participant must share
its candidate onion address with the next participant through Extra peer. Then
contacts can be learned and persisted. No central hosted service is required,
but no software can discover an entirely unknown network without an initial
contact. Unconnected miners may create isolated forks.

## Verification and limits

Internal tests include the 13 instrumented native suites plus signer-console
recovery/spending; current and final height-zero identity/isolation; two candidate nodes matching tip/UTXO state and reciprocal rejection of test/candidate files and clients; independent
Python SHA3-512 reproduction of the named genesis proof and exact target math;
26 real-C# / simulated-Tor discovery checks, including three distinct native
lanes; rolling-quota/four-lane selection and a full 64-entry provenance-store reload; 12 backend lifecycle checks
from a bracket-containing directory; ambiguous submission/retry/confirmation
cleanup fixtures; native encrypted backup restore/verification through the actual PowerShell coordinator, generated candidate backend lifecycle, PowerShell syntax and archive CRC/SHA-256 checks.
Windows x64 binaries for both profiles are cross-compiled with strict warnings;
this does not mean they were run on Windows. Linux tests use PowerShell 7.4.6,
not Windows PowerShell 5.1. Tor in transport fixtures is simulated. ASan/UBSan
checks do not instrument the static OpenSSL library; LeakSanitizer is disabled.

Remaining gates: native Windows/Unicode/restore/dialog/Tor/suspend tests,
unrelated participant bootstrap and matured candidate spending, sustained
adversarial network testing, independent review, build reproduction and publisher
signing. Multiple onion services do not establish Sybil resistance. No claim of
a completed security audit or a live public network is made.
