#!/usr/bin/env python3
"""Package a public GlazeBoot build and emulator image for glaze-os.com.

The input directory contains kernel.bin, romfs.img, GlazeBoot.exe,
glaze.cfg and glaze.bin. Vendor firmware and private Wi-Fi seeds are never
copied into the public archive. See docs/WEBSITE.md for the release build.
"""
import argparse
import hashlib
import json
import re
import struct
from datetime import date
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

PREPARE_CARD = '''#!/usr/bin/env python3
"""Create a private Wi-Fi seed in your own Glaze CF folder, if absent."""
import os
import sys
from pathlib import Path

folder = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "Glaze"
if not all((folder / name).is_file() for name in ("GlazeBoot.exe", "kernel.bin", "romfs.img", "glaze.cfg")):
    raise SystemExit("Choose the Glaze folder containing the extracted boot files.")
seed = folder / "glaze.seed"
if seed.exists():
    if seed.stat().st_size != 32:
        raise SystemExit("Existing glaze.seed is not 32 bytes; it was left untouched.")
    print("Existing per-card Wi-Fi seed preserved.")
else:
    with seed.open("xb") as output:
        output.write(os.urandom(32))
    print("Created a private 32-byte Wi-Fi seed. Keep it on your own card.")
'''


def romfs_entries(blob):
    if len(blob) < 8 or blob[:4] != b'GZRF':
        raise ValueError('romfs.img is not a Glaze archive')
    count = struct.unpack_from('<I', blob, 4)[0]
    if count > (len(blob) - 8) // 56:
        raise ValueError('romfs.img has a truncated file table')
    names = set()
    for i in range(count):
        raw, offset, size = struct.unpack_from('<48sII', blob, 8 + 56 * i)
        name = raw.split(b'\0', 1)[0].decode('utf-8')
        if offset + size > len(blob):
            raise ValueError(f'romfs entry exceeds the archive: {name}')
        names.add(name)
    return names


def readme(revision, release_date):
    return f'''Glaze for the Casio Cassiopeia BE-300
Release: {release_date} / {revision[:7]}
Source revision: {revision}

REAL BE-300: BOOT FROM WINDOWS CE
1. Extract this ZIP on your computer.
2. Optionally run "python3 prepare-card.py" to create your own private
   Wi-Fi seed. Never share or reuse another device's glaze.seed. Existing
   seeds are preserved. Ethernet and open networks do not require this step.
3. Copy the Glaze folder to the root of a FAT-formatted CompactFlash card.
4. In Windows CE, open \\Storage Card\\Glaze\\GlazeBoot.exe and choose Boot.
   GlazeBoot loads kernel.bin and romfs.img from the same folder.

No NAND flashing is needed. / is a temporary 2 MiB RAM disk: its files,
installed apps and settings disappear on reset. Save persistent files on
/Card. Windows CE's NAND volume is exposed as /NAND read-only (nand=ro).
The hardware archive includes App Store, Browser and Settings to leave RAM
for applications. Other apps can be installed from the App Store.

To return to Windows CE, fully remove power, including the battery, and
restart. Glaze overwrites Windows CE's RAM object store; a warm reset alone
can loop, and data kept only in that object store is lost. The stock NAND
image and files on the CF card are not overwritten by the default boot.

HEADPHONES AND NETWORKING
Headphone PCM output and music volume control are verified on real hardware.
The internal piezo buzzer uses a separate controller, outside this PCM driver.
Use Settings > Sounds > Volume; releasing the slider changes all playback.
AmbiCom NE2000 Ethernet and XI-825 open-network operation are hardware-tested.
The single CF socket can accept a network card after Glaze has loaded.

The WL54-CF driver is present, but this public ZIP omits the extracted vendor
firmware. To use that card, obtain your own supported vendor package, follow
docs/WL54CF.md in the source, and rebuild with WL54CF=1. WEP/WPA/WPA2 support
is emulator-tested; physical-card security interoperability is not claimed.
Windows CE applications, vendor DLLs and commercial game ROMs are not bundled.

EMULATOR
Emulator/glaze.bin is a Glaze NAND image, not a Windows CE restore image.
Run a copy, since the emulator writes changes back to its NAND image:
  be300 --nand Emulator/glaze.bin --rtc-host-time --ne2000 --audio
This image contains no shared WPA entropy seed. For WPA testing, build a
private image from source or use a prepared per-card seed and the required
firmware-enabled build. Normal Ethernet operation needs neither.

CHECKSUMS AND SOURCE
SHA256SUMS lists every other file in this ZIP. SOURCE.txt links the exact
corresponding source and build instructions. LICENSE and licenses/ contain
license notices. Project: https://glaze-os.com/
'''


def package(build_dir, site_dir, revision, release_date, repository):
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('revision must be a full Git commit SHA')
    when = date.fromisoformat(release_date)
    files = {f'Glaze/{name}': (build_dir / name).read_bytes()
             for name in ('GlazeBoot.exe', 'kernel.bin', 'romfs.img', 'glaze.cfg')}
    files['Emulator/glaze.bin'] = (build_dir / 'glaze.bin').read_bytes()
    if not files['Glaze/GlazeBoot.exe'].startswith(b'MZ'):
        raise ValueError('GlazeBoot.exe is not a Windows executable')
    if revision[:7].encode() not in files['Glaze/kernel.bin']:
        raise ValueError('kernel.bin does not identify the requested revision')
    if len(files['Emulator/glaze.bin']) != 16 * 1024 * 1024:
        raise ValueError('glaze.bin must be a 16 MiB NAND image')
    nand_names = romfs_entries(files['Emulator/glaze.bin'][0x40000:0x500000])
    if any(name.startswith('firmware/') or name == 'bin/ktest' for name in nand_names):
        raise ValueError('public NAND image contains vendor firmware or kernel tests')
    names = romfs_entries(files['Glaze/romfs.img'])
    if any(name.startswith('firmware/') or name == 'bin/ktest' for name in names):
        raise ValueError('public boot archive contains vendor firmware or kernel tests')
    required = {f'bin/{name}' for name in ('springboard', 'appstore', 'browser', 'settings', 'audio', 'net', 'fs')}
    if not required.issubset(names):
        raise ValueError('boot archive is missing a required application or service')
    config = dict(line.split('=', 1) for line in files['Glaze/glaze.cfg'].decode().splitlines()
                  if '=' in line and not line.lstrip().startswith('#'))
    if config.get('nand') != 'ro':
        raise ValueError('public boot configuration must use nand=ro')
    files['README.txt'] = readme(revision, release_date).encode()
    files['prepare-card.py'] = PREPARE_CARD.encode()
    source_url = f'https://github.com/jroark/glaze-os/archive/{revision}.zip'
    files['SOURCE.txt'] = (f'Corresponding source: {source_url}\n'
                           f'Commit: {revision}\n\n'
                           'Build instructions: docs/GLAZEBOOT.md and docs/WEBSITE.md\n'
                           './build.sh all glazeboot INIT=bin/springboard INPUT_SCRIPT= WL54CF=0\n'
                           'make glazeboot-exe BE300_DEV=/path/to/be300-dev\n'
                           'The public emulator filesystem is created without a shared Wi-Fi seed.\n').encode()
    files['LICENSE'] = (repository / 'LICENSE').read_bytes()
    files['licenses/third_party/README.md'] = (repository / 'third_party/README.md').read_bytes()
    files['licenses/fonts/VeraCopyright.txt'] = (repository / 'assets/fonts/VeraCopyright.txt').read_bytes()
    for path in sorted((repository / 'third_party').rglob('*')):
        if path.is_file() and path.name.lower().startswith(('license', 'licence', 'copying', 'copyright', 'notice')):
            files['licenses/' + path.relative_to(repository).as_posix()] = path.read_bytes()
    files['SHA256SUMS'] = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n'
                                  for name, data in sorted(files.items())).encode()
    downloads = site_dir / 'downloads'
    downloads.mkdir(parents=True, exist_ok=True)
    filename = f'glaze-{release_date}-{revision[:7]}.zip'
    archive = downloads / filename
    with ZipFile(archive, 'w', compression=ZIP_DEFLATED, compresslevel=9) as output:
        for name, data in sorted(files.items()):
            info = ZipInfo(name, (when.year, when.month, when.day, 0, 0, 0))
            info.compress_type = ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            output.writestr(info, data)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (downloads / (filename + '.sha256')).write_text(f'{digest}  {filename}\n')
    metadata = dict(date=release_date, revision=revision, build_id=revision[:7], filename=filename,
                    size_bytes=archive.stat().st_size, sha256=digest, source_url=source_url,
                    vendor_firmware_included=False, shared_wifi_seed_included=False)
    (downloads / 'latest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--site-dir', type=Path, default=Path('site'))
    parser.add_argument('--revision', required=True)
    parser.add_argument('--date', default=date.today().isoformat())
    args = parser.parse_args()
    repository = Path(__file__).resolve().parent.parent
    metadata = package(args.build_dir, args.site_dir, args.revision, args.date, repository)
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
