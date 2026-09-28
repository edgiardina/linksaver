#!/usr/bin/env python3
"""Packs Linksaver into a single self-contained .scr.

Appends linksaver.exe, SDL2.dll, the extracted game assets, the reference
saves and the default settings to the launcher (src/launcher/launcher.c),
which unpacks them to %LOCALAPPDATA%\\Linksaver on first run.

The result contains game data extracted from your ROM. Keep it for your own
machines; don't publish it.

Usage: python tools/pack_linksaver.py [launcher.exe] [output.scr]
"""
import glob
import os
import struct
import sys
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def main():
    launcher = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'linksaver_launcher.exe')
    output = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'dist', 'Linksaver.scr')

    files = [
        ('linksaver.exe', 'linksaver.exe'),
        ('SDL2.dll', 'SDL2.dll'),
        ('zelda3_assets.dat', 'zelda3_assets.dat'),
        ('zelda3.ini', 'zelda3.ini'),
    ]
    for path in sorted(glob.glob(os.path.join(ROOT, 'saves', 'ref', '*.sav'))):
        name = 'saves/ref/' + os.path.basename(path)
        files.append((name, os.path.relpath(path, ROOT)))

    missing = [src for _, src in files if not os.path.exists(os.path.join(ROOT, src))]
    if missing:
        sys.exit('Missing: %s (build with build_linksaver.bat and extract assets first)' % ', '.join(missing))

    with open(launcher, 'rb') as f:
        out = bytearray(f.read())
    payload_offset = len(out)
    payload = bytearray()
    for name, src in files:
        with open(os.path.join(ROOT, src), 'rb') as f:
            data = f.read()
        encoded = name.encode('utf-8')
        payload += struct.pack('<H', len(encoded)) + encoded + struct.pack('<I', len(data)) + data

    # The hash names the unpack folder, so a rebuilt .scr unpacks fresh.
    payload_hash = zlib.crc32(payload) & 0xffffffff
    out += payload
    out += struct.pack('<III', payload_offset, len(files), payload_hash) + b'LSPAYLD1'

    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, 'wb') as f:
        f.write(out)
    print('Wrote %s (%.1f MB, %d files, id %08x)' % (output, len(out) / 1e6, len(files), payload_hash))


if __name__ == '__main__':
    main()
