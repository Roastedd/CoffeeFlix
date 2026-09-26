#!/usr/bin/env python3
"""Offers a signed CoffeeFlix build to Wii Us on this network (developer updates).

Run through tools/dev-update.sh. The Wii U asks for a server with a UDP broadcast on port 47291,
this answers with the port of its web server, and the Wii U reads dev.json (version, size,
SHA-256, signature, notes) and downloads coffeeflix.wuhb. Only builds signed with the key
matching the one built into the app install.
"""

import argparse
import hashlib
import http.server
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

DISCOVERY_PORT = 47291
ASK = b"COFFEEFLIX-DEV?"
ANSWER = "COFFEEFLIX-DEV {}\n"


def sign(path, key):
    """The file's SHA-256 signed with the PEM private key (ECDSA, DER), as hex; checked before use."""
    sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key, path], check=True, capture_output=True).stdout
    with tempfile.TemporaryDirectory() as tmp:
        pub, sig_path = os.path.join(tmp, "pub.pem"), os.path.join(tmp, "sig")
        subprocess.run(["openssl", "ec", "-in", key, "-pubout", "-out", pub], check=True, capture_output=True)
        with open(sig_path, "wb") as f:
            f.write(sig)
        subprocess.run(["openssl", "dgst", "-sha256", "-verify", pub, "-signature", sig_path, path], check=True, capture_output=True)
    return sig.hex()


def local_ip():
    """This computer's address on the network (no packet is sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]
    except OSError:
        return "this computer"
    finally:
        s.close()


def answer_discovery(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", DISCOVERY_PORT))
    seen = set()
    while True:
        data, addr = s.recvfrom(256)
        if data.strip() != ASK:
            continue
        s.sendto(ANSWER.format(port).encode(), addr)
        if addr[0] not in seen:
            seen.add(addr[0])
            print(f"  {addr[0]} found this computer", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--wuhb", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--notes", default="")
    ap.add_argument("--key", required=True)
    ap.add_argument("--port", type=int, default=47292)
    args = ap.parse_args()

    # A copy, so building again doesn't change the file while a Wii U downloads it.
    serve_dir = tempfile.mkdtemp(prefix="coffeeflix-dev-")
    wuhb = os.path.join(serve_dir, "coffeeflix.wuhb")
    shutil.copyfile(args.wuhb, wuhb)
    with open(wuhb, "rb") as f:
        if f.read(4) != b"WUHB":
            sys.exit(f"{args.wuhb} isn't a Wii U app bundle")
    digest = hashlib.sha256()
    with open(wuhb, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    manifest = json.dumps({
        "version": args.version,
        "size": os.path.getsize(wuhb),
        "sha256": digest.hexdigest(),
        "signature": sign(wuhb, args.key),
        "notes": args.notes,
    }).encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/dev.json":
                body, kind = manifest, "application/json"
            elif self.path == "/coffeeflix.wuhb":
                print(f"  {self.client_address[0]} is downloading {args.version}", flush=True)
                body, kind = None, "application/octet-stream"
            else:
                self.send_error(404)
                return
            size = len(body) if body is not None else os.path.getsize(wuhb)
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(size))
            self.end_headers()
            try:
                if body is not None:
                    self.wfile.write(body)
                else:
                    with open(wuhb, "rb") as f:
                        shutil.copyfileobj(f, self.wfile, 256 * 1024)
                    print(f"  {self.client_address[0]} has the whole build", flush=True)
            except (BrokenPipeError, ConnectionResetError):
                print(f"  {self.client_address[0]} stopped the download", flush=True)

        def log_message(self, *a):
            pass

    server = http.server.ThreadingHTTPServer(("", args.port), Handler)
    threading.Thread(target=answer_discovery, args=(args.port,), daemon=True).start()
    print(f"\nOffering CoffeeFlix {args.version} ({os.path.getsize(wuhb) / 1e6:.1f} MB) from {local_ip()}:{args.port}")
    print("On the Wii U: Settings > Updates, with Developer updates on. Ctrl-C stops this.\n", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        shutil.rmtree(serve_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
