# QBTC desktop review — preview 0.9.0-preview.4

The supplied report was checked against current source. The test-v2 chain,
wallet storage, genesis, difficulty, mining rules and future launch plan are unchanged.

| Finding | Assessment and correction |
| --- | --- |
| Single startup check | Confirmed. Authenticated local status retries for up to 120 seconds; process exit and panel cancellation stop the wait. |
| Pinned Tor URL lifetime | A single-source availability risk, not evidence that this particular URL has already disappeared. Added the official archive hierarchy as fallback with the exact same mandatory SHA-256. Neither host availability nor perpetual support is guaranteed. |
| Download slowness/visibility | Replaced the PowerShell progress pipeline with an asynchronous download, one-second MB status updates, cancellation checks and a 600-second limit per source. ProgressPreference is suppressed and restored. |
| Missing payment retry | Confirmed. Added Retry signed payment; preserves transaction bytes and checks their hash, never signs a replacement. A durable reservation is written before signing, new payments are blocked while unresolved, and a per-wallet action lock prevents concurrent coordinators. |
| IP disagreement | Backend already binds loopback. Removed obsolete adapter detection and hidden-IP polling prerequisite; submissions also use loopback. |
| Empty state crashes | Confirmed. Empty address/onion/mode reads become empty strings. Medium remains the default for an empty mining-mode file. |
| Disk error demotes peer | Confirmed. Persistence errors are handled separately after network verification; reachable peers retain verified status. |
| Backup overwrite dialog | Confirmed UX mismatch. Dialog explains that a NEW filename is required; overwrite prompt disabled and existing destination rejected before launching backup. Existing backups remain protected. |
| Old version on ports | Added a finite allowlist of hashes extracted from previously shipped binaries, still requiring the exact test-chain path. Unknown processes are never stopped. An active old panel/session must still be closed; locks are not bypassed. |
| Minor issues | Stop-only messages corrected; custom $args renamed; one prior node/stdout and stderr log retained; NOTICE corrected. |

## Validation

- PowerShell helper/real coordinator fixtures: delayed startup, cancellation, dead process, bounded timeout, first-source failure and fallback, mandatory checksum rejection, restored progress setting, previous log preservation, empty persisted state.
- Payment coordinator with fake signer/submitter: ambiguous submission retained, duplicate signing blocked, modified file refused, identical retry without signing, cancellation removes an unsigned reservation.
- Real C# manager with simulated Tor and real native height-zero node: 20 discovery checks, including a failing address-store path that does not demote reachable peers.
- Backend lifecycle: 12 checks including stop, owner exit, listener cleanup and preservation of the external peer.
- Existing pinned-runtime extraction/rejection regression and PowerShell syntax checks.
- Rebuilt ZIP manifests, SHA-256 and page/document links checked after packaging.

These are Linux PowerShell/C# fixtures; Windows dialogs, Windows Job Objects and
real Tor connectivity for this new preview still require native verification.
The native core binaries are unchanged from the prior instrumented regression pass.
This is an internal review, not an independent security audit.

An unresolved payment can remain pending after a lost acknowledgement even if
already known or mined. Retry is idempotent at the transaction level; automatic
chain-status reconciliation and coin selection reservations after acceptance
remain future wallet work. Do not bypass the pending record to create a replacement.
