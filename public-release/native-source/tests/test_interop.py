"""Cross-check pure ML-DSA-65 between native C++ OpenSSL and Python cryptography.
Both providers use OpenSSL: this tests API/encoding interoperability, not two
independent cryptographic algorithm implementations. No private keys hit disk.
"""
import pathlib
import subprocess
import tempfile
from cryptography.hazmat.primitives.asymmetric.mldsa import MLDSA65PrivateKey, MLDSA65PublicKey
from cryptography.hazmat.backends.openssl.backend import backend

binary = pathlib.Path(__file__).resolve().parents[1] / "build" / "qbtc-crypto-interop"
with tempfile.TemporaryDirectory(prefix="qbtc-interop-") as name:
    directory = pathlib.Path(name)
    subprocess.run([binary, "emit", directory], check=True)
    key = MLDSA65PublicKey.from_public_bytes((directory / "cpp-public.bin").read_bytes())
    key.verify((directory / "cpp-signature.bin").read_bytes(), (directory / "cpp-message.bin").read_bytes())
    print("PASS C++-to-Python ML-DSA verification")
    private = MLDSA65PrivateKey.generate()
    message = b"QBTC/python-interop/1"
    (directory / "python-public.bin").write_bytes(private.public_key().public_bytes_raw())
    (directory / "python-message.bin").write_bytes(message)
    (directory / "python-signature.bin").write_bytes(private.sign(message))
    subprocess.run([binary, "verify", directory], check=True)
print("Python provider:", backend.openssl_version_text())
