"""Save sync (runtime/save_sync.c) writes what MacNativeGames' cloud_save.m reads.

    .venv/bin/python tests/save_sync_check.py --exe build/v143/totk

Runs `totk --probe-save-sync` in a temporary folder it keeps, then checks the sync folder the probe left as the other
port would read it: sync.plist parses as an Apple property list (plistlib) with the keys and types cloud_save.m
read_cloud_meta checks, and its hash equals cloud_save.m save_hash over the folder, recomputed here as written there:
SHA-256 over the regular files sorted by relative path, without storage/, .DS_Store, .<name>.icloud and the top-level
sync.plist; per file the path and a NUL, the size as a native (little-endian) uint64, the bytes. Needs no game files.
"""
import argparse
import hashlib
import os
import plistlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

UID = '3e69ca33dea6dc10cb00165c15cd5098'


def mng_hash(folder):
    files = []
    for dirpath, _, names in os.walk(folder):
        for name in names:
            rel = Path(dirpath, name).relative_to(folder).as_posix()
            if name == '.DS_Store' or rel == 'sync.plist' or (name.startswith('.') and name.endswith('.icloud')):
                continue
            if rel.startswith('storage/') or not Path(dirpath, name).is_file():
                continue
            files.append(rel)
    h = hashlib.sha256()
    for rel in sorted(files):
        data = (folder / rel).read_bytes()
        h.update(rel.encode() + b'\0')
        h.update(struct.pack('<Q', len(data)))
        h.update(data)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    args = ap.parse_args()
    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, TOTK_SAVE_SYNC_PROBE_DIR=tmp)
        r = subprocess.run([args.exe, '--probe-save-sync'], env=env, capture_output=True, timeout=300)
        out = (r.stdout + r.stderr).decode('utf-8', 'replace')
        if r.returncode:
            print(out)
            bad.append(f'--probe-save-sync exited {r.returncode}')
        cur = Path(tmp) / 'sync' / 'Tears of the Kingdom' / 'Users' / UID / 'Saves' / 'current'
        try:
            meta = plistlib.loads((cur / 'sync.plist').read_bytes())
        except Exception as e:  # noqa: BLE001 - any parse failure is the finding
            meta = {}
            bad.append(f'sync.plist does not parse as a property list: {e}')
        want = {'uid': str, 'gen': int, 'device': str, 'time': float, 'hash': str}
        for key, typ in want.items():
            if not isinstance(meta.get(key), typ):
                bad.append(f'sync.plist {key}: {meta.get(key)!r} is not {typ.__name__}')
        if meta.get('uid') != UID:
            bad.append(f'sync.plist uid {meta.get("uid")!r}')
        if meta and meta.get('hash') != mng_hash(cur):
            bad.append(f'sync.plist hash {meta.get("hash")} differs from cloud_save.m save_hash {mng_hash(cur)}')
        state = Path(tmp) / 'b' / 'cloud-sync' / f'{UID}.plist'
        try:
            s = plistlib.loads(state.read_bytes())
            if s.get('gen') != meta.get('gen') or s.get('hash') != meta.get('hash'):
                bad.append(f'the uploader\'s state {s} does not name the folder\'s generation and hash')
        except Exception as e:  # noqa: BLE001
            bad.append(f'state file does not parse: {e}')
    for b in bad:
        print('FAIL:', b)
    print('save_sync_check: FAILED' if bad else f'save_sync_check: passed (generation {meta["gen"]} from '
          f'{meta["device"]}, hash as cloud_save.m computes it)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
