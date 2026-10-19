#!/usr/bin/env python3
"""Convert a PEM bundle of root certificates into the browser's trust store:
the certificates' DER encodings, concatenated (romfs ssl/roots.der, read by
apps/browser/tls.c).

The checked-in assets/ssl/roots.der comes from certifi's cacert.pem, which
is Mozilla's root store limited to roots trusted for TLS servers:

    curl -L -o cacert.pem \\
        https://raw.githubusercontent.com/certifi/python-certifi/<tag>/certifi/cacert.pem
    tools/mkroots.py cacert.pem assets/ssl/roots.der

usage: mkroots.py IN.pem OUT.der
"""

import base64
import sys

BEGIN = "-----BEGIN CERTIFICATE-----"
END = "-----END CERTIFICATE-----"


def main() -> int:
    src, out = sys.argv[1], sys.argv[2]
    certs = []
    body = None
    with open(src, encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if line == BEGIN:
                body = []
            elif line == END:
                if body is None:
                    raise SystemExit(f"{src}: END without BEGIN")
                der = base64.b64decode("".join(body))
                if der[0] != 0x30:
                    raise SystemExit(f"{src}: certificate {len(certs) + 1} is not DER")
                certs.append(der)
                body = None
            elif body is not None:
                body.append(line)
    if not certs:
        raise SystemExit(f"{src}: no certificates")
    with open(out, "wb") as f:
        for der in certs:
            f.write(der)
    print(f"{out}: {len(certs)} certificates, {sum(map(len, certs))} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
