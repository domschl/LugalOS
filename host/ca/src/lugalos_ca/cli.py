"""lugalos-ca: the owner's certificate authority for LugalOS nodes.

    lugalos-ca init [--name NAME]                       # once: a P-256 root key and certificate
    lugalos-ca issue --tcp HOST --keyfile KEY [--days N] # a node's certificate, over 9P
    lugalos-ca show                                      # where the root certificate is, its fingerprint

`issue` asks the node for a key if it has none (`keygen` to /dev/tls), reads its request
(/dev/tlscsr), checks the request's signature, signs a certificate for the names in it, writes it
to /dev/tlscert and reads /dev/tls back to see it took. The node's key never leaves the node.

The root key stays in ~/.config/lugalos/ca/ (mode 0600). Install ca.crt.pem as a trusted root on
every device that opens the nodes' admin pages (doc/admin/tls.md says how, per system).
"""

from __future__ import annotations

import argparse
import datetime
import ipaddress
import os
import secrets
import sys
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

DEFAULT_DIR = Path.home() / ".config" / "lugalos" / "ca"
# iOS and macOS refuse TLS server certificates valid for longer than 825 days, even from a root the
# user installed.
DEFAULT_DAYS = 825


def ca_paths(d: Path) -> tuple[Path, Path]:
    return d / "ca.key.pem", d / "ca.crt.pem"


def cmd_init(a: argparse.Namespace) -> int:
    d = Path(a.dir)
    key_path, crt_path = ca_paths(d)
    if key_path.exists() and not a.force:
        print(f"{key_path} exists; --force makes a new root (every node's certificate then needs reissuing)",
              file=sys.stderr)
        return 1
    d.mkdir(parents=True, exist_ok=True)
    os.chmod(d, 0o700)
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, a.name),
                      x509.NameAttribute(NameOID.ORGANIZATION_NAME, "LugalOS")])
    now = datetime.datetime.now(datetime.timezone.utc)
    pub = key.public_key()
    cert = (x509.CertificateBuilder()
            .subject_name(name).issuer_name(name).public_key(pub)
            .serial_number(int.from_bytes(secrets.token_bytes(16), "big") >> 1)
            .not_valid_before(now - datetime.timedelta(hours=1))
            .not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
            .add_extension(x509.KeyUsage(digital_signature=False, content_commitment=False, key_encipherment=False,
                                         data_encipherment=False, key_agreement=False, key_cert_sign=True,
                                         crl_sign=True, encipher_only=False, decipher_only=False), critical=True)
            .add_extension(x509.SubjectKeyIdentifier.from_public_key(pub), critical=False)
            .sign(key, hashes.SHA256()))
    fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    crt_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    print(f"root key:         {key_path}")
    print(f"root certificate: {crt_path}  (install this one as trusted on your devices)")
    print(f"fingerprint:      {cert.fingerprint(hashes.SHA256()).hex(':')}")
    return 0


def load_ca(d: Path) -> tuple[ec.EllipticCurvePrivateKey, x509.Certificate]:
    key_path, crt_path = ca_paths(d)
    if not key_path.exists():
        sys.exit(f"no CA in {d}: run `lugalos-ca init` first")
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey):
        sys.exit(f"{key_path} is not an EC key")
    return key, x509.load_pem_x509_certificate(crt_path.read_bytes())


def build_cert(csr: x509.CertificateSigningRequest, ca_key: ec.EllipticCurvePrivateKey,
               ca_cert: x509.Certificate, days: int, also: list[str] | None = None) -> x509.Certificate:
    """A server certificate for what the request names (its subject, its subjectAltNames, its key),
    plus the names in `also`, which the CA's owner chose to add."""
    if not csr.is_signature_valid:
        raise ValueError("the request's signature does not verify")
    pub = csr.public_key()
    if not isinstance(pub, ec.EllipticCurvePublicKey) or not isinstance(pub.curve, ec.SECP256R1):
        raise ValueError("the request is not for a P-256 key")
    try:
        san = csr.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
    except x509.ExtensionNotFound:
        raise ValueError("the request names no subjectAltName") from None
    for n in san:
        if isinstance(n, x509.DNSName) and not n.value.endswith(".local"):
            raise ValueError(f"the request names {n.value!r}; a node's names end in .local")
        if isinstance(n, x509.IPAddress) and not n.value.is_private:
            raise ValueError(f"the request names the public address {n.value}")
    names: list[x509.GeneralName] = list(san)
    for extra in also or []:
        try:
            names.append(x509.IPAddress(ipaddress.ip_address(extra)))
        except ValueError:
            names.append(x509.DNSName(extra))
    san = x509.SubjectAlternativeName(names)
    now = datetime.datetime.now(datetime.timezone.utc)
    ski = ca_cert.extensions.get_extension_for_class(x509.SubjectKeyIdentifier).value
    return (x509.CertificateBuilder()
            .subject_name(csr.subject).issuer_name(ca_cert.subject).public_key(pub)
            .serial_number(int.from_bytes(secrets.token_bytes(16), "big") >> 1)
            .not_valid_before(now - datetime.timedelta(hours=1))
            .not_valid_after(now + datetime.timedelta(days=days))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False, key_encipherment=False,
                                         data_encipherment=False, key_agreement=False, key_cert_sign=False,
                                         crl_sign=False, encipher_only=False, decipher_only=False), critical=True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .add_extension(san, critical=False)
            .add_extension(x509.SubjectKeyIdentifier.from_public_key(pub), critical=False)
            .add_extension(x509.AuthorityKeyIdentifier.from_issuer_subject_key_identifier(ski), critical=False)
            .sign(ca_key, hashes.SHA256()))


def status(text: bytes) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in text.decode(errors="replace").splitlines():
        k, _, v = line.partition("=")
        out[k] = v
    return out


def cmd_issue(a: argparse.Namespace) -> int:
    from p9lib import Session, connect_tcp

    ca_key, ca_cert = load_ca(Path(a.dir))
    key = bytes.fromhex(Path(a.keyfile).read_text().strip()) if a.keyfile else bytes.fromhex(a.key)
    host, _, port = a.tcp.partition(":")
    sess = Session(connect_tcp(host, int(port) if port else 564), key=key, uname=a.uname)
    try:
        st = status(sess.read("/dev/tls"))
        board = status(sess.read("/proc/board"))
        print(f"node: {board.get('name', '?')} -- {board.get('hardware', '?')}, uid {board.get('uid', '?')}")
        if st.get("key") != "p256" or a.rekey:
            print("asking the node for a new key" if a.rekey else "the node has no key: asking it for one")
            sess.write("/dev/tls", b"rekey\n" if a.rekey else b"keygen\n", create=False, truncate=False)
        pem = sess.read("/dev/tlscsr")
        csr = x509.load_pem_x509_csr(pem)
        if a.save_csr:
            Path(a.save_csr).write_bytes(pem)
        cert = build_cert(csr, ca_key, ca_cert, a.days, a.also)
        names = ", ".join(str(n.value) for n in cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value)
        out = cert.public_bytes(serialization.Encoding.PEM)
        if a.save_cert:
            Path(a.save_cert).write_bytes(out)
        sess.write("/dev/tlscert", out, create=False, truncate=False)
        st = status(sess.read("/dev/tls"))
    finally:
        sess.close()
    if st.get("cert") != "installed" or st.get("matches_key") != "yes":
        print(f"the node did not take the certificate: {st}", file=sys.stderr)
        return 1
    print(f"issued and installed: {names}, until {st.get('not_after')} (the node's key {st.get('key_fingerprint')})")
    return 0


def cmd_show(a: argparse.Namespace) -> int:
    _, crt = load_ca(Path(a.dir))
    print(f"root certificate: {ca_paths(Path(a.dir))[1]}")
    print(f"subject:          {crt.subject.rfc4514_string()}")
    print(f"valid until:      {crt.not_valid_after_utc:%Y-%m-%d}")
    print(f"fingerprint:      {crt.fingerprint(hashes.SHA256()).hex(':')}")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="lugalos-ca", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dir", default=str(DEFAULT_DIR), help=f"the CA's directory (default {DEFAULT_DIR})")
    sub = p.add_subparsers(dest="cmd", required=True)
    pi = sub.add_parser("init", help="make the root key and certificate")
    pi.add_argument("--name", default=f"LugalOS CA ({os.environ.get('USER', 'owner')})")
    pi.add_argument("--force", action="store_true", help="replace an existing root")
    ps = sub.add_parser("issue", help="issue a node's certificate over 9P")
    ps.add_argument("--tcp", required=True, metavar="HOST[:PORT]")
    g = ps.add_mutually_exclusive_group(required=True)
    g.add_argument("--keyfile", help="file holding the 9P key (an admin grant), hex")
    g.add_argument("--key", help="the 9P key, hex")
    ps.add_argument("--uname", default="lugal", help="the grant's name (default lugal)")
    ps.add_argument("--days", type=int, default=DEFAULT_DAYS)
    ps.add_argument("--rekey", action="store_true", help="have the node make a new key first")
    ps.add_argument("--also", action="append", metavar="NAME|IP",
                    help="another name or address for the certificate (a port-forwarded one, say); repeatable")
    ps.add_argument("--save-csr", metavar="FILE", help="keep the node's request")
    ps.add_argument("--save-cert", metavar="FILE", help="keep the issued certificate")
    sub.add_parser("show", help="the root certificate's path and fingerprint")
    a = p.parse_args(argv)
    try:
        return {"init": cmd_init, "issue": cmd_issue, "show": cmd_show}[a.cmd](a)
    except ValueError as e:
        print(f"refused: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
