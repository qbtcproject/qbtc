"""Fetch exact OpenSSL sources for this release; Python 3.12+, no third-party deps."""
import hashlib
import pathlib
import tarfile
import urllib.request

VERSION = "3.5.9"
SHA256 = "603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a"
root = pathlib.Path(__file__).resolve().parents[1] / ".deps"
archive = root / f"openssl-{VERSION}.tar.gz"
source = root / f"openssl-{VERSION}"
root.mkdir(exist_ok=True)
if not archive.exists():
    url = f"https://github.com/openssl/openssl/releases/download/openssl-{VERSION}/openssl-{VERSION}.tar.gz"
    temporary = archive.with_suffix(".download")
    try:
        with urllib.request.urlopen(url, timeout=60) as response, temporary.open("wb") as output:
            while data := response.read(1024 * 1024):
                output.write(data)
        temporary.replace(archive)
    finally:
        temporary.unlink(missing_ok=True)
actual = hashlib.sha256(archive.read_bytes()).hexdigest()
if actual != SHA256:
    raise SystemExit("OpenSSL source checksum mismatch; refusing to use archive")
if not source.exists():
    with tarfile.open(archive) as package:
        package.extractall(root, filter="data")
print(f"Verified OpenSSL {VERSION}: {actual}")
