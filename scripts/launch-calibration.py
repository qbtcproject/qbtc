"""Offline launch-target candidate. Does not activate a network or alter any chain."""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import re

SPACE = 1 << 512
OLD_TARGET = (1 << 492) - 1
SPACING = 600
INTERVAL = 2016

def proposed_target(hashes_per_second):
    rate = Decimal(str(hashes_per_second))
    if not rate.is_finite() or rate <= 0:
        raise ValueError('positive finite hashrate required')
    # Exact rational arithmetic: target + 1 <= 2^512 / (H * 600).
    numerator, denominator = rate.as_integer_ratio()
    target = (SPACE * denominator) // (numerator * SPACING) - 1
    if target < 1:
        raise ValueError('hashrate exceeds the representable target range')
    return min(OLD_TARGET, target)

def read_samples(paths, expected=2):
    if len(paths) != expected:
        raise ValueError(f'exactly {expected} independent-PC reports required')
    rates, runs, fingerprints = [], set(), set()
    reports = []
    for path in paths:
        path = Path(path)
        if path.stat().st_size > 16384:
            raise ValueError('report too large')
        record = json.loads(path.read_text(encoding='utf-8-sig'))
        if record.get('schema') != 'qbtc-cpu-calibration-v1' or record.get('network') != 'none' or record.get('benchmark_only') is not True or record.get('mode') != 'high':
            raise ValueError('High-mode isolated calibration report required')
        hashes, micros, threads = (record.get(n) for n in ('hashes', 'elapsed_us', 'threads'))
        if any(type(n) is not int for n in (hashes, micros, threads)) or not 0 < hashes < 1 << 64 or not 30_000_000 <= micros <= 125_000_000 or not 1 <= threads <= 256:
            raise ValueError('invalid measurement range')
        run = record.get('run_id', '')
        fingerprint = record.get('peer_binary_sha256', '')
        if not re.fullmatch('[a-f0-9]{32}', run) or run in runs:
            raise ValueError('missing or repeated run ID')
        if not re.fullmatch('[a-f0-9]{64}', fingerprint):
            raise ValueError('missing miner binary identity')
        runs.add(run); fingerprints.add(fingerprint)
        rate = Decimal(hashes) * 1_000_000 / Decimal(micros)
        rates.append(rate)
        reports.append({'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'run_id': run, 'hps': str(rate)})
    if len(fingerprints) != 1:
        raise ValueError('both PCs must use the same miner binary')
    total = sum(rates, Decimal(0))
    target = proposed_target(total)
    expected_time = Decimal(SPACE) / Decimal(target + 1) / total
    return {
        'status': 'candidate_only_not_an_active_network',
        'reference': 'combined high-mode throughput of two user PCs',
        'reported_independent_pcs': expected,
        'independence_note': 'Run IDs prevent accidental duplication; they cannot prove two physical PCs.',
        'peer_binary_sha256': next(iter(fingerprints)),
        'aggregate_hps': str(total), 'target_hex': f'{target:0128x}',
        'expected_block_seconds_at_reference': str(expected_time),
        'target_block_seconds': SPACING, 'retarget_blocks': INTERVAL,
        'retarget_max_change': 4, 'initial_reward_qbtc': 50, 'halving_blocks': 210000,
        'maximum_supply_qbtc': 21000000,
        'estimated_issuance_two_days_at_reference_qbtc': 14400,
        'not_a_hard_issuance_limit': True,
        'official_genesis': None, 'official_network_id': None,
        'experimental_balances_imported': False, 'measurements': reports,
    }

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reports', nargs=2, type=Path)
    args = parser.parse_args()
    print(json.dumps(read_samples(args.reports), indent=2))
