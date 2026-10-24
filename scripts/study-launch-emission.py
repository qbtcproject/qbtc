"""Seeded honest-miner scenario model, not a proof of network security or an issuance ceiling."""
import importlib.util
import json
import math
from pathlib import Path
import random

spec = importlib.util.spec_from_file_location('calibration', Path(__file__).with_name('launch-calibration.py'))
calibration = importlib.util.module_from_spec(spec); spec.loader.exec_module(calibration)
PERIOD = 2016 * 600

def simulate(reference_hps, ratio, seed=0, horizon=172800):
    rng = random.Random(seed)
    target = calibration.proposed_target(reference_hps)
    now = 0.0; height = 0; issued_atoms = 0; timestamps = [0]
    first_target = target
    while height < 250000:
        timestamp = max(int(now), timestamps[-1] + 1)
        if timestamp > now + 7200:
            now = timestamp - 7200
        if now >= horizon:
            break
        next_height = height + 1
        if next_height % 2016 == 0:
            span = timestamps[-1] - timestamps[next_height - 2016]
            elapsed = min(PERIOD * 4, max(PERIOD // 4, span))
            target = max(1, min(calibration.OLD_TARGET, target * elapsed // PERIOD))
        expected = calibration.SPACE / (target + 1) / (float(reference_hps) * ratio)
        now += rng.expovariate(1 / expected)
        if now > horizon:
            break
        height = next_height
        timestamps.append(timestamp)
        issued_atoms += (50 * 100000000) >> (height // 210000)
    return {'hashrate_ratio': ratio, 'blocks': height, 'issued_qbtc': issued_atoms / 100000000,
            'initial_expected_block_seconds': calibration.SPACE / (first_target + 1) / (float(reference_hps) * ratio)}

def study(reference_hps=1000000):
    scenarios = []
    for ratio in (.5, 1, 4, 100, 10000, 1000000):
        runs = [simulate(reference_hps, ratio, seed) for seed in range(16)]
        scenarios.append({'hashrate_ratio': ratio,
            'min_issued_qbtc': min(x['issued_qbtc'] for x in runs),
            'max_issued_qbtc': max(x['issued_qbtc'] for x in runs),
            'mean_issued_qbtc': sum(x['issued_qbtc'] for x in runs) / len(runs)})
    return {'reference_hps': reference_hps, 'reference_is_example_not_user_measurement': True,
        'horizon_seconds': 172800, 'seeds_per_scenario': 16, 'scenarios': scenarios,
        'extreme_counterexample': simulate(reference_hps, 1e24),
        'limitations': ['Not a consensus implementation or adversarial network simulation.',
            'Honest timestamps follow the current miner; includes its local 7200-second future admission bound.',
            'No latency, competing blocks, orphan rates, clock attacks or hardware differences are modeled.',
            'Fixed 2016-block adjustment cannot promise a ten-minute average immediately after a hashrate change.',
            'Calibrating two PCs does not impose a hard upper bound on network-wide issuance.']}

if __name__ == '__main__':
    print(json.dumps(study(), indent=2))
