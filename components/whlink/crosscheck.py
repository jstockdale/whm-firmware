#!/usr/bin/env python3
"""Cross-check wh-link reference crypto (C) against pyca/cryptography.
Reads `dump_vectors` output on stdin; recomputes each case independently and
asserts the C output matches. Exits non-zero on any mismatch."""
import sys, hashlib, hmac
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.kdf.hkdf import HKDF, HKDFExpand
from cryptography.hazmat.primitives import hashes

def uh(s):
    return b"" if s == "-" else bytes.fromhex(s)

def clamp(sk):
    b = bytearray(sk); b[0] &= 248; b[31] &= 127; b[31] |= 64; return bytes(b)

npass = nfail = 0
def ok(cond, label):
    global npass, nfail
    if cond: npass += 1
    else:
        nfail += 1
        print(f"  MISMATCH {label}")

for line in sys.stdin:
    f = line.split()
    if not f: continue
    kind = f[0]
    if kind == "SHA256":
        msg, out = uh(f[1]), uh(f[2])
        ok(hashlib.sha256(msg).digest() == out, "SHA256")
    elif kind == "HMAC":
        key, msg, out = uh(f[1]), uh(f[2]), uh(f[3])
        ok(hmac.new(key, msg, hashlib.sha256).digest() == out, "HMAC")
    elif kind == "HKDF":
        salt, ikm, info, ol, out = uh(f[1]), uh(f[2]), uh(f[3]), int(f[4]), uh(f[5])
        h = HKDF(algorithm=hashes.SHA256(), length=ol,
                 salt=(salt if salt else None), info=(info if info else b""))
        ok(h.derive(ikm) == out, "HKDF")
    elif kind == "AEAD":
        key, nonce, aad, pt, ct, tag = (uh(f[1]),uh(f[2]),uh(f[3]),uh(f[4]),uh(f[5]),uh(f[6]))
        ref = ChaCha20Poly1305(key).encrypt(nonce, pt, aad if aad else None)
        ok(ref == ct + tag, "AEAD-seal")
        dec = ChaCha20Poly1305(key).decrypt(nonce, ct + tag, aad if aad else None)
        ok(dec == pt, "AEAD-open")
    elif kind == "X25519B":
        sk, pub = uh(f[1]), uh(f[2])
        p = X25519PrivateKey.from_private_bytes(sk)
        from cryptography.hazmat.primitives import serialization
        ref = p.public_key().public_bytes(serialization.Encoding.Raw,
                                          serialization.PublicFormat.Raw)
        ok(ref == pub, "X25519-base")
    elif kind == "X25519":
        sa, sb, sh = uh(f[1]), uh(f[2]), uh(f[3])
        pa = X25519PrivateKey.from_private_bytes(sa)
        pb_priv = X25519PrivateKey.from_private_bytes(sb)
        from cryptography.hazmat.primitives import serialization
        pb_pub_raw = pb_priv.public_key().public_bytes(serialization.Encoding.Raw,
                                                       serialization.PublicFormat.Raw)
        ref = pa.exchange(X25519PublicKey.from_public_bytes(pb_pub_raw))
        ok(ref == sh, "X25519-DH")
    else:
        print(f"  UNKNOWN LINE {kind}"); nfail += 1

print(f"crosscheck: {npass} agreements, {nfail} mismatches")
sys.exit(0 if nfail == 0 else 1)
