import importlib.util
import json
from pathlib import Path
import tempfile
from decimal import Decimal

ROOT = Path(__file__).parent
def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/(name+'.py'))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
c=load('launch-calibration');s=load('study-launch-emission')
for rate in (100000, 1000000, 1000000000):
    target=c.proposed_target(rate)
    assert (target+1)*rate*600 <= c.SPACE < (target+2)*rate*600
    assert 1 <= target <= c.OLD_TARGET
assert c.proposed_target(2000000) < c.proposed_target(1000000)
with tempfile.TemporaryDirectory() as temporary:
    folder=Path(temporary)
    base={'schema':'qbtc-cpu-calibration-v1','network':'none','benchmark_only':True,'mode':'high',
          'threads':7,'elapsed_us':30000000,'peer_binary_sha256':'a'*64}
    paths=[]
    for i,hashes in enumerate((15000000,24000000)):
        path=folder/(str(i)+'.json');path.write_text(json.dumps(dict(base,hashes=hashes,run_id=str(i)*32)));paths.append(path)
    report=c.read_samples(paths)
    assert Decimal(report['aggregate_hps']) == 1300000
    assert abs(Decimal(report['expected_block_seconds_at_reference'])-600) < Decimal('1e-20')
    assert report['official_genesis'] is None and report['experimental_balances_imported'] is False
    for alteration in ({'run_id':'0'*32},{'peer_binary_sha256':'b'*64},{'mode':'low'}, {'elapsed_us':10}):
        paths[1].write_text(json.dumps(dict(base,hashes=24000000,run_id='1'*32,**alteration))) if 'run_id' not in alteration else paths[1].write_text(json.dumps(dict(base,hashes=24000000,**alteration)))
        try:c.read_samples(paths)
        except ValueError:pass
        else:raise AssertionError(alteration)
baseline=[s.simulate(1000000,1,seed)['issued_qbtc'] for seed in range(32)]
assert 11000 < sum(baseline)/len(baseline) < 18000
assert s.simulate(1000000,1e24)['issued_qbtc'] > 3000000
print('PASS 512-bit target arithmetic, two-PC aggregation, duplicate/build/mode/duration rejection, no launch activation, baseline scenarios, counterexample to a hard issuance guarantee')
