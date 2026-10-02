#!/usr/bin/env python3
"""Offers a signed CoffeeFlix build to Wii Us on this network (developer updates).

Run through tools/dev-update.sh. The Wii U asks for a server with a UDP broadcast on port 47291,
this answers with the port of its web server, and the Wii U reads dev.json (version, size,
SHA-256, signature, notes) and downloads coffeeflix.wuhb. Only builds signed with the key
matching the one built into the app install.

While this runs, a Wii U with developer updates on also sends its log here as it goes (POST
/log): each run of the app gets its own file in logs/, and logs/wiiu-latest.log is the newest.
Warnings, errors and the performance lines are shown here too.
"""

import argparse
import hashlib
import http.server
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

DISCOVERY_PORT = 47291
ASK = b"COFFEEFLIX-DEV?"
ANSWER = "COFFEEFLIX-DEV {}\n"
MAX_LOG_POST = 1 << 20
# Log lines worth showing as they come: problems, and the performance summaries.
SHOWN = re.compile(r"\[(WARNING|ERROR)\]|\[(CPU|Frames)\]|Playback:")
CONTROL = re.compile("[\x00-\x08\x0b-\x1f\x7f-\x9f]")


def printable(text):
    """What a Wii U sent, safe to print: no terminal control sequences."""
    return CONTROL.sub("?", text)


class Logs:
    """Each run's log as the Wii U sends it, in a file of its own."""

    def __init__(self, folder):
        self.folder = folder
        self.lock = threading.Lock()
        self.files = {}  # client address -> (run, file)

    def add(self, ip, run, version, text):
        with self.lock:
            run_now, f = self.files.get(ip, (None, None))
            if run_now != run:
                if f:
                    f.close()
                os.makedirs(self.folder, exist_ok=True)
                stamp = time.strftime("%Y%m%d-%H%M%S")
                path = os.path.join(self.folder, f"wiiu-{stamp}.log")
                n = 2
                while os.path.exists(path):
                    path = os.path.join(self.folder, f"wiiu-{stamp}-{n}.log")
                    n += 1
                f = open(path, "a", encoding="utf-8")
                self.files[ip] = (run, f)
                latest = os.path.join(self.folder, "wiiu-latest.log")
                link = latest + ".new"
                if os.path.lexists(link):
                    os.remove(link)
                os.symlink(os.path.basename(path), link)
                os.replace(link, latest)
                print(f"  {ip} is sending its log (CoffeeFlix {printable(version)}) to {path}", flush=True)
            f.write(text)
            f.flush()
        for line in text.splitlines():
            if SHOWN.search(line):
                print("  | " + printable(line), flush=True)


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


def prepare_build(folder, build_id, name, bundle, version, notes, key):
    if not re.fullmatch(r"[a-z][a-z0-9-]{0,47}", build_id):
        raise ValueError("Invalid build ID")
    if not name or len(name) > 64 or not version or len(version) > 128:
        raise ValueError("Invalid build name or version")
    # Freeze each binary for this server session, even if another build starts on the computer.
    wuhb = os.path.join(folder, build_id + ".wuhb")
    shutil.copyfile(bundle, wuhb)
    with open(wuhb, "rb") as f:
        if f.read(4) != b"WUHB":
            raise ValueError(f"{bundle} is not a Wii U bundle")
    size = os.path.getsize(wuhb)
    if not (1 << 20) <= size <= (256 << 20):
        raise ValueError("Build must be between 1 and 256 MiB")
    with open(wuhb, "rb") as f:
        digest = hashlib.file_digest(f, "sha256").hexdigest()
    info = {"id": build_id, "name": name, "version": version, "size": size,
            "sha256": digest, "signature": sign(wuhb, key), "notes": notes}
    return info, wuhb


def project_info(project):
    def define(path, name):
        with open(path, encoding="utf-8") as f:
            match = re.search(r'^#define ' + name + r' "([^"\n]+)"$', f.read(), re.MULTILINE)
        if not match:
            raise ValueError(f"Missing {name} in {path}")
        return match.group(1)
    project = os.path.abspath(project)
    header = os.path.join(project, "src", "core", "build_variant.hpp")
    return (define(header, "APP_BUILD_ID"), define(header, "APP_BUILD_NAME"),
            os.path.join(project, "coffeeflix.wuhb"),
            define(os.path.join(project, "build", "app_version.h"), "APP_VERSION"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--wuhb", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--notes", default="")
    ap.add_argument("--key", required=True)
    ap.add_argument("--build-id", default="coffeeflix")
    ap.add_argument("--build-name", default="CoffeeFlix")
    ap.add_argument("--also-project", action="append", default=[], help="also offer this checkout's existing build")
    ap.add_argument("--port", type=int, default=47292)
    ap.add_argument("--bind", default="", help="interface to listen on (default: all)")
    ap.add_argument("--no-discovery", action="store_true", help="disable UDP discovery, for isolated local tests")
    ap.add_argument("--logs", default="logs", help="where the Wii U's logs go")
    args = ap.parse_args()
    logs = Logs(args.logs)

    with tempfile.TemporaryDirectory(prefix="coffeeflix-dev-") as serve_dir:
        specs = [(args.build_id, args.build_name, args.wuhb, args.version, args.notes)]
        for project in args.also_project:
            build_id, name, bundle, version = project_info(project)
            specs.append((build_id, name, bundle, version, f"{name} development build\nVersion {version}"))
        if len(specs) > 8 or len({s[0] for s in specs}) != len(specs):
            raise ValueError("Offer at most eight builds, each with a different ID")
        builds, routes = [], {}
        for spec in specs:
            info, path = prepare_build(serve_dir, *spec, args.key)
            builds.append(info)
            routes[f"/builds/{info['id']}/{info['sha256']}/coffeeflix.wuhb"] = (info, path)
        primary = builds[0]
        primary_path = os.path.join(serve_dir, primary['id'] + ".wuhb")
        # Old installed builds still see the primary image and can bootstrap the new chooser.
        legacy_manifest = json.dumps(primary).encode()
        payload = json.dumps({"schema": 1, "builds": builds}, separators=(",", ":"))
        catalog_path = os.path.join(serve_dir, "catalog.json")
        with open(catalog_path, "wb") as f:
            f.write(payload.encode())
        catalog = json.dumps({"catalog": payload, "signature": sign(catalog_path, args.key)}).encode()

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self):
                try:
                    size = int(self.headers.get("Content-Length", ""))
                except ValueError:
                    size = -1
                if self.path != "/log" or not 0 <= size <= MAX_LOG_POST:
                    self.close_connection = True
                    self.send_error(404 if self.path != "/log" else 413)
                    return
                text = self.rfile.read(size).decode("utf-8", "replace")
                logs.add(self.client_address[0], self.headers.get("X-CoffeeFlix-Run", ""),
                         self.headers.get("X-CoffeeFlix-Version", "?")[:64], text)
                self.send_response(204)
                self.end_headers()

            def do_GET(self):
                path, info = None, None
                if self.path == "/builds.json":
                    body, kind = catalog, "application/json"
                elif self.path == "/dev.json":
                    body, kind = legacy_manifest, "application/json"
                elif self.path == "/coffeeflix.wuhb":
                    body, kind, path, info = None, "application/octet-stream", primary_path, primary
                elif self.path in routes:
                    info, path = routes[self.path]
                    body, kind = None, "application/octet-stream"
                else:
                    self.send_error(404)
                    return
                size = len(body) if body is not None else info['size']
                if info:
                    print(f"  {self.client_address[0]} is downloading {info['name']} {info['version']}", flush=True)
                self.send_response(200)
                self.send_header("Content-Type", kind)
                self.send_header("Content-Length", str(size))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                try:
                    if body is not None:
                        self.wfile.write(body)
                    else:
                        with open(path, "rb") as f:
                            shutil.copyfileobj(f, self.wfile, 256 * 1024)
                        print(f"  {self.client_address[0]} has the whole {info['name']} build", flush=True)
                except (BrokenPipeError, ConnectionResetError):
                    print(f"  {self.client_address[0]} stopped the download", flush=True)

            def log_message(self, *a):
                pass

        server = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
        if not args.no_discovery:
            threading.Thread(target=answer_discovery, args=(server.server_port,), daemon=True).start()
        for info in builds:
            print(f"Offering {info['name']} {info['version']} ({info['size'] / 1e6:.1f} MB)", flush=True)
        print(f"Server: {args.bind or local_ip()}:{server.server_port}; older clients receive {primary['name']}", flush=True)
        print("On Wii U: Settings > Updates > Build to install (Developer updates on). Ctrl-C stops this.", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()


if __name__ == "__main__":
    main()
