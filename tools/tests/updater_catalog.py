#!/usr/bin/env python3
"""Exercise the real dev server/updater with an ephemeral key and isolated fake bundles.
make -f desktop.mk -f tools/tests/updater_catalog.mk updater-tests
python3 tools/tests/updater_catalog.py [--ui]
The server binds only loopback on a random port and does not broadcast to the Wii U.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import time
import urllib.request


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ui', action='store_true')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[2]
    os.chdir(root)
    folder = Path(tempfile.mkdtemp(prefix='coffeeflix-selector-test-'))
    print('Artifacts:', folder, flush=True)
    key, pub = folder / 'key.pem', folder / 'public.pem'
    subprocess.run(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', str(key)], check=True, capture_output=True)
    subprocess.run(['openssl', 'ec', '-in', str(key), '-pubout', '-out', str(pub)], check=True, capture_output=True)
    first = b'WUHB' + b'A' * (1024 * 1024)
    second = b'WUHB' + b'B' * (1024 * 1024)
    (folder / 'primary.wuhb').write_bytes(first)
    (folder / 'running.wuhb').write_bytes(first)
    other = folder / 'other'
    (other / 'build').mkdir(parents=True)
    (other / 'src/core').mkdir(parents=True)
    (other / 'coffeeflix.wuhb').write_bytes(second)
    (other / 'build/app_version.h').write_text('#define APP_VERSION "test-other"\n')
    (other / 'src/core/build_variant.hpp').write_text('#define APP_BUILD_ID "otherbuild"\n#define APP_BUILD_NAME "Other build"\n')
    server_log = folder / 'server.log'
    with server_log.open('w') as log:
        proc = subprocess.Popen(['python3', 'tools/dev_server.py', '--wuhb', str(folder / 'primary.wuhb'),
            '--version', 'test-coffeeflix', '--key', str(key), '--also-project', str(other), '--port', '0',
            '--bind', '127.0.0.1', '--no-discovery', '--logs', str(folder / 'logs')], stdout=log, stderr=subprocess.STDOUT)
    try:
        server = None
        for _ in range(100):
            match = re.search(r'Server: (127\.0\.0\.1:\d+)', server_log.read_text())
            if match:
                server = match.group(1)
                break
            if proc.poll() is not None:
                raise RuntimeError(server_log.read_text())
            time.sleep(0.05)
        assert server, server_log.read_text()
        with urllib.request.urlopen('http://' + server + '/builds.json', timeout=5) as r:
            envelope = json.load(r)
        original = json.loads(envelope['catalog'])
        spec = importlib.util.spec_from_file_location('dev_server', root / 'tools/dev_server.py')
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        malformed = []
        d = copy.deepcopy(original); d['schema'] = 2; malformed.append(('bad-schema', d))
        d = copy.deepcopy(original); d['builds'].append(d['builds'][0]); malformed.append(('duplicate-id', d))
        d = copy.deepcopy(original); d['builds'][0]['signature'] = 'abcd'; malformed.append(('bad-binary-signature', d))
        for name, data in malformed:
            payload = json.dumps(data, separators=(',', ':'))
            path = folder / (name + '.payload')
            path.write_text(payload)
            (folder / (name + '.json')).write_text(json.dumps({'catalog': payload, 'signature': mod.sign(str(path), str(key))}))
        # Rebuilding the source file must not change this server session's snapshot.
        (folder / 'primary.wuhb').write_bytes(b'WUHB' + b'Z' * (1024 * 1024))
        subprocess.run(['build-desktop/updater-catalog-test', server, str(folder), str(pub)], check=True, timeout=35)
        if args.ui:
            ui = folder / 'ui'; ui.mkdir()
            bundle = ui / 'running.wuhb'; bundle.write_bytes(first)
            (ui / 'coffeeflix.json').write_text(json.dumps({'settings': {'update_dev_unlocked': True,
                'update_dev': True, 'update_dev_build': 'coffeeflix', 'update_auto': False, 'ui_sounds': False}}))
            script = folder / 'ui.txt'
            script.write_text(f'''wait 150
shot {folder}/up-to-date.png
tap 700 180
wait 30
shot {folder}/chooser.png
press DOWN
wait 10
press A
wait 150
shot {folder}/selected.png
tap 1130 620
wait 150
shot {folder}/ready.png
tap 700 180
wait 25
shot {folder}/ready-locked.png
tap 872 620
wait 50
tap 700 180
wait 25
press A
wait 150
shot {folder}/back-to-coffeeflix.png
quit
''')
            env = dict(os.environ, SDL_VIDEODRIVER='cocoa', SDL_RENDER_DRIVER='metal', SDL_AUDIODRIVER='dummy',
                COFFEEFLIX_DATA=str(ui), COFFEEFLIX_BUNDLE=str(bundle), COFFEEFLIX_DEV_SERVER=server,
                COFFEEFLIX_SCRIPT=str(script), COFFEEFLIX_LANGUAGE='en')
            with (folder / 'ui.log').open('w') as log:
                subprocess.run(['build-desktop/updater-catalog-test', '--ui', str(pub)], env=env,
                    stdout=log, stderr=subprocess.STDOUT, check=True, timeout=65)
            saved = json.loads((ui / 'coffeeflix.json').read_text())
            assert saved['settings']['update_dev_build'] == 'coffeeflix'
            assert bundle.read_bytes() == first and not Path(str(bundle) + '.download').exists()
            assert 'is downloaded and matches' in (folder / 'ui.log').read_text(), 'Download was not exercised'
            assert (folder / 'back-to-coffeeflix.png').exists()
        print('All updater selector tests passed', flush=True)
    finally:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=10)


if __name__ == '__main__':
    main()
