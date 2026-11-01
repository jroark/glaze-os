"""Stage a matching build and package it on the target host. Never accesses USB."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import struct
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]


def package_infos():
    """Package Info files here and in the built sibling app repositories."""
    repos = [ROOT, Path(os.environ.get('APPS_DIR', ROOT.parent / 'glaze-apps')),
             Path(os.environ.get('EMU_DIR', ROOT.parent / 'glaze-emulators'))]
    for repo in repos[1:]:
        if not (repo / 'packages').is_dir():
            print(f'note: {repo} not found; its apps are left out of the installer', file=sys.stderr)
    return [info for repo in repos for info in sorted((repo / 'packages').glob('*/Info'))]


def validate_update(path, build_id):
    if path is None:
        raise ValueError('A signed matching OS update is required (--os-update).')
    data = Path(path).read_bytes()
    if len(data) < 512 or data[:8] != b'GZOTA1\0\0':
        raise ValueError('Invalid signed OS update')
    header, size, count, spl = struct.unpack_from('<IIII', data, 8)
    version = data[24:40].split(b'\0')[0].decode('ascii')
    build = data[40:72].split(b'\0')[0].decode('ascii')
    if (header != 512 or size != len(data) or not 12 <= count <= 315 or
            build != build_id or version != (ROOT / 'VERSION').read_text().strip() or
            hashlib.sha256(data[512:]).digest() != data[72:104]):
        raise ValueError('OS update does not match the release')
    # Verify with the same pinned release key trusted by the device.
    with tempfile.TemporaryDirectory() as folder:
        folder = Path(folder)
        (folder / 'key.der').write_bytes(bytes.fromhex('302a300506032b6570032100') + (ROOT / 'config/ota_pubkey.bin').read_bytes())
        (folder / 'header').write_bytes(data[:448])
        (folder / 'signature').write_bytes(data[448:512])
        candidates = [os.environ['OPENSSL']] if os.environ.get('OPENSSL') else ['openssl', '/opt/homebrew/bin/openssl', '/usr/local/opt/openssl@3/bin/openssl', '/opt/homebrew/opt/openssl@3/bin/openssl']
        openssl = None
        for candidate in candidates:
            binary = shutil.which(candidate)
            if binary and subprocess.run([binary, 'version'], capture_output=True, text=True).stdout.startswith('OpenSSL 3.'):
                openssl = binary
                break
        if openssl is None:
            raise ValueError('Packaging signed updates requires OpenSSL 3; set OPENSSL.')
        subprocess.run([openssl, 'pkeyutl', '-verify', '-pubin', '-keyform', 'DER',
                        '-inkey', str(folder / 'key.der'), '-rawin', '-in', str(folder / 'header'),
                        '-sigfile', str(folder / 'signature')], check=True, capture_output=True)
    # Check the signed ROMFS against the standard OS image being packaged.
    blocks = []
    for i in range(count):
        offset, length, crc = struct.unpack_from('<III', data, 512 + i * 12)
        if offset < 512 + count * 12 or offset + length > len(data):
            raise ValueError('Invalid update block')
        block = data[offset:offset+length]
        if length != 16384:
            block = zlib.decompress(block, -15)
        if len(block) != 16384 or zlib.crc32(block) != crc:
            raise ValueError('Invalid update block checksum')
        blocks.append(block)
    payload = b''.join(blocks)
    logical = (ROOT / 'out/glaze.bin').read_bytes()
    if payload != logical[0x14000:0x14000+len(payload)]:
        raise ValueError('Signed update differs from bundled NAND kernel/ROMFS')
    return {'version': version, 'build': build, 'spl_min': spl}


def stage_resources(out, native_dir, build_id, ext='', os_update=None):
    os_metadata = validate_update(os_update, build_id)
    resources = out / 'resources'
    if resources.exists():
        shutil.rmtree(resources)
    (resources / 'bin').mkdir(parents=True)
    for name in ('glaze-installer-usb', 'nand-inventory', 'mkfsimg'):
        shutil.copy2(native_dir / (name + ext), resources / 'bin')
    (resources / 'ram').mkdir()
    for name in ('GlazeBoot.exe', 'UsbProbe.exe', 'kernel.bin', 'romfs.img'):
        shutil.copy2(ROOT / 'out/glazeboot' / name, resources / 'ram')
    shutil.copy2(ROOT / 'out/glaze.bin', resources)
    shutil.copy2(os_update, resources / 'update.gzu')
    shutil.copy2(ROOT / 'out/pkg/maintenance.gzp', resources / 'maintenance.gzp')
    shutil.copy2(ROOT / 'LICENSE', resources / 'LICENSE')
    shutil.copytree(ROOT / 'userdata', resources / 'userdata')
    (resources / 'tools').mkdir()
    for name in ('physical_nand.py', 'nand_transaction.py'):
        shutil.copy2(ROOT / 'tools' / name, resources / 'tools')
    # The complete offline catalog is shipped even when optional apps are deselected.
    catalog = []
    draw = (ROOT / 'servers/springboard/draw.c').read_text()
    for name, program in re.findall(r'\.label = "([^"]+)".*?\.path = "bin/([^"]+)"', draw):
        catalog.append({'id': 'builtin:' + program, 'name': name, 'program': program,
                        'kind': 'builtin', 'required': program in ('files', 'settings'), 'default': True})
    catalog.append({'id': 'builtin:wince', 'name': 'Windows CE compatibility', 'program': 'wince',
                    'kind': 'builtin', 'required': False, 'default': False})
    (resources / 'packages').mkdir()
    for info in package_infos():
        metadata = dict(line.split('=', 1) for line in info.read_text().splitlines() if '=' in line)
        program = metadata['id']
        if program in ('hello', 'maintenance'):
            continue
        # Each repository packs its packages into its own out/pkg.
        shutil.copy2(info.parents[2] / 'out/pkg' / (program + '.gzp'), resources / 'packages')
        catalog.append({'id': 'package:' + program, 'name': metadata['name'], 'program': program,
                        'kind': 'package', 'required': False, 'default': False})
    catalog.append({'id': 'firmware:wl54cf', 'name': 'WL54-CF Wi-Fi firmware', 'program': 'wl54cf',
                    'kind': 'firmware', 'required': False, 'default': False,
                    'files': ['firmware/cf8385_helper.bin', 'firmware/cf8385.bin']})
    (resources / 'apps.json').write_text(json.dumps(catalog, indent=2))
    notices = resources / 'licenses'
    notices.mkdir()
    # Only the exact redistributable Linux pair is included, with provenance
    # and its complete notice. The vendor-CAB helper is deliberately rejected.
    firmware_manifest = json.loads((ROOT / 'firmware/wl54cf-linux.json').read_text())
    (resources / 'firmware').mkdir()
    for name, entry in firmware_manifest['files'].items():
        if not name.startswith('libertas/'):
            continue
        source = ROOT / 'firmware/wl54cf' / Path(name).name
        data = source.read_bytes()
        if len(data) != entry['size_bytes'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise ValueError('Unexpected WL54-CF firmware: ' + str(source))
        (resources / 'firmware' / source.name).write_bytes(data)
    notice = (ROOT / 'firmware/LICENCE.Marvell').read_bytes()
    expected = firmware_manifest['files']['LICENCE.Marvell']
    if len(notice) != expected['size_bytes'] or hashlib.sha256(notice).hexdigest() != expected['sha256']:
        raise ValueError('Unexpected Marvell redistribution notice')
    (notices / 'LICENCE.Marvell').write_bytes(notice)
    shutil.copy2(ROOT / 'firmware/wl54cf-linux.json', notices)
    for component in ('littlefs', 'dhara'):
        for path in (ROOT / 'third_party' / component).glob('*'):
            if path.is_file() and path.name.lower().startswith(('license', 'copying')):
                shutil.copy2(path, notices / (component + '-' + path.name))
    manifest = {'version': 1, 'build_id': build_id, 'os': os_metadata, 'files': {}}
    for path in sorted(resources.rglob('*')):
        if path.is_file() and path.parent != resources / 'bin':
            manifest['files'][str(path.relative_to(resources)).replace('\\', '/')] = hashlib.sha256(path.read_bytes()).hexdigest()
    (resources / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    return resources


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build-id', required=True, help='BUILD_ID used to compile the bundled kernel')
    p.add_argument('--os-update', type=Path, required=True, help='Signed .gzu matching the bundled build')
    p.add_argument('--native-dir', type=Path, default=ROOT / 'out/installer-native')
    p.add_argument('--stage-only', action='store_true')
    args = p.parse_args()
    out = ROOT / 'out/installer'
    if sys.platform != 'darwin':
        p.error('Use package_windows.py to build the offline Windows installer')
    ext = ''
    resources = stage_resources(out, args.native_dir, args.build_id, ext, args.os_update)
    if args.stage_only:
        return
    sep = ':'
    command = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--name', 'Glaze Installer',
               '--onedir', '--distpath', str(out / 'dist'), '--workpath', str(out / 'build'),
               '--specpath', str(out), '--add-data', str(resources) + sep + 'resources',
               '--hidden-import', 'tkinter', '--hidden-import', 'tkinter.ttk',
               '--hidden-import', 'tkinter.filedialog', '--hidden-import', 'tkinter.messagebox']
    for path in (resources / 'bin').iterdir():
        command += ['--add-binary', str(path) + sep + 'resources/bin']
    command += ['--windowed', '--osx-bundle-identifier', 'com.glazeos.installer']
    command += [str(ROOT / 'installer/app.py')]
    subprocess.run(command, check=True)
    arch = platform.machine().lower()
    layout = out / 'disk-layout'
    if layout.exists():
        shutil.rmtree(layout)
    layout.mkdir()
    shutil.copytree(out / 'dist/Glaze Installer.app', layout / 'Glaze Installer.app', symlinks=True)
    (layout / 'Applications').symlink_to('/Applications')
    shutil.copy2(ROOT / 'installer/START-HERE.txt', layout)
    subprocess.run(['hdiutil', 'create', '-volname', 'Glaze Installer', '-srcfolder',
                    str(layout), '-ov', str(out / f'Glaze-Installer-macos-{arch}.dmg')], check=True)
    paths = list(out.glob('*.dmg')) + list(out.glob('*.exe'))
    (out / 'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest() + '  ' + f.name + '\n' for f in paths))


if __name__ == '__main__':
    main()
