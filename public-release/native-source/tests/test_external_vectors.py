"""Run QBTC's actual verifier against the empty-context ML-DSA-65 Wycheproof
vectors distributed with the pinned OpenSSL source release. Public fixtures only.
"""
import os
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = pathlib.Path(os.environ.get("OPENSSL_ROOT", root / ".deps/openssl-3.5.9"))
vectors = source / "test/recipes/30-test_evp_data/evppkey_ml_dsa_65_wycheproof_verify.txt"
content = vectors.read_text()
keys = {}
for line in content.splitlines():
    if line.startswith("PublicKeyRaw = "):
        name, algorithm, public = line.split(" = ", 1)[1].split(":", 2)
        assert algorithm == "ML-DSA-65"
        keys[name] = bytes.fromhex(public)
valid = invalid = skipped = 0
with tempfile.TemporaryDirectory(prefix="qbtc-wycheproof-") as name:
    directory = pathlib.Path(name)
    for block in content.split("\n\n"):
        if "Verify-Message-Public = ML-DSA-65:" not in block:
            continue
        controls = [line.removeprefix("Ctrl = ") for line in block.splitlines() if line.startswith("Ctrl = ")]
        if any(c not in ["message-encoding:1", "deterministic:1", "hexcontext-string:"] for c in controls):
            skipped += 1
            continue
        fields = dict(line.split(" = ", 1) for line in block.splitlines() if " = " in line and not line.startswith("#"))
        public = keys[fields["Verify-Message-Public"].split(":", 1)[1]]
        message, signature = bytes.fromhex(fields["Input"]), bytes.fromhex(fields["Output"])
        expected = "Result" not in fields
        (directory / "python-public.bin").write_bytes(public)
        (directory / "python-message.bin").write_bytes(message)
        (directory / "python-signature.bin").write_bytes(signature)
        result = subprocess.run([root / "build/qbtc-crypto-interop", "verify", directory], capture_output=True)
        if expected:
            assert result.returncode == 0, result.stderr.decode()
            valid += 1
        else:
            assert result.returncode == 1 and b"signature failed" in result.stderr, result.stderr.decode()
            invalid += 1
assert valid and invalid
print(f"PASS external Wycheproof ML-DSA-65: {valid} valid accepted, {invalid} invalid rejected; {skipped} nonempty-context cases excluded (QBTC uses empty context)")
print("Source: OpenSSL 3.5.9, Wycheproof commit 8e7fa6f87e6993d7b613cf48b46512a32df8084a")
