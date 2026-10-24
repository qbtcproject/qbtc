"""Check the reserved official plan without activating a network or mining a block."""
from pathlib import Path
import json,re
root=Path(__file__).resolve().parents[1]
release=root/'public-release' if (root/'public-release').is_dir() else root
p=json.loads((release/'FINAL-LAUNCH-PLAN.json').read_text())
if (root/'dist/science/final-launch-plan.json').exists():
 assert p==json.loads((root/'dist/science/final-launch-plan.json').read_text())
assert p['reference_hps']==2026000 and p['reference_mhps']=='2.026'
assert p['expected_attempts_per_block']==1215600000
n=int(p['proposed_initial_target'],16)
assert (n+1)*1215600000 <= (1<<512) < (n+2)*1215600000
assert p['genesis_name']=='fuckgreed' and p['genesis_height']==0 and p['first_rewarded_block_height']==1
assert p['first_difficulty_adjustment_height']==2016 and p['halving_blocks']==210000
assert p['status']=='candidate_frozen_not_launched' and p['official_network_launched'] is False
assert p['official_genesis_hash'] and p['official_network_id']=='0x51424d33'
current=json.loads((release/'MAINNET-GENESIS.json').read_text())
assert current['network_id']=='0x51424d32'
assert current['genesis_hash']=='000006d71412362513fb97c48c0a1371b0edd3e7e6a29e91a8318f889181e5dd1255621ebcc717083a1a135c57c12b6c3885e2e0dbfac410d43089fbb49ed73c'
s=(release/'native-source/src/consensus.cpp').read_text()
assert current['initial_rewarded_block_target']!=p['proposed_initial_target']
assert all('0x'+current['initial_rewarded_block_target'][i:i+2] in s for i in range(0,128,2))
import hashlib,struct
f=json.loads((release/'FINAL-GENESIS.json').read_text())
assert f['network_id']=='0x51424d33' and f['genesis_name']=='fuckgreed'
root_hash=hashlib.sha3_512(f['genesis_name_commitment'].encode()).digest()
assert root_hash.hex()==f['genesis_root']
raw=b'QBTC/pow/1'+struct.pack('<IIQ',1,int(f['network_id'],16),0)+bytes(64)+root_hash+bytes.fromhex(f['genesis_target'])+struct.pack('<QQ',f['genesis_timestamp'],f['genesis_nonce'])
assert hashlib.sha3_512(raw).hexdigest()==p['official_genesis_hash']==f['genesis_hash']
assert int(f['genesis_hash'],16)<=int(f['genesis_target'],16)==n
assert f['genesis_target']==f['initial_rewarded_block_target'] and f['genesis_reward_atoms']==0
assert current['genesis_hash']!=f['genesis_hash']
print('PASS frozen named candidate genesis proof, exact 2.026 MH/s / 600s target from genesis, distinct identity and current test genesis retained')
