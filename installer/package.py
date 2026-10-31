"""Stage a matching build and package it on the target host. Never accesses USB."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def stage_resources(out, native_dir, build_id, ext=''):
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
                    'kind': 'builtin', 'required': False, 'default': True})
    (resources / 'packages').mkdir()
    for info in sorted((ROOT / 'packages').glob('*/Info')):
        metadata = dict(line.split('=', 1) for line in info.read_text().splitlines() if '=' in line)
        program = metadata['id']
        shutil.copy2(ROOT / 'out/pkg' / (program + '.gzp'), resources / 'packages')
        catalog.append({'id': 'package:' + program, 'name': metadata['name'], 'program': program,
                        'kind': 'package', 'required': False, 'default': False})
    (resources / 'apps.json').write_text(json.dumps(catalog, indent=2))
    notices = resources / 'licenses'
    notices.mkdir()
    for component in ('littlefs', 'dhara'):
        for path in (ROOT / 'third_party' / component).glob('*'):
            if path.is_file() and path.name.lower().startswith(('license', 'copying')):
                shutil.copy2(path, notices / (component + '-' + path.name))
    manifest = {'version': 1, 'build_id': build_id, 'files': {}}
    for path in sorted(resources.rglob('*')):
        if path.is_file() and path.parent != resources / 'bin':
            manifest['files'][str(path.relative_to(resources)).replace('\\', '/')] = hashlib.sha256(path.read_bytes()).hexdigest()
    (resources / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    return resources


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build-id', required=True, help='BUILD_ID used to compile the bundled kernel')
    p.add_argument('--native-dir', type=Path, default=ROOT / 'out/installer-native')
    p.add_argument('--stage-only', action='store_true')
    args = p.parse_args()
    out = ROOT / 'out/installer'
    if sys.platform != 'darwin':
        p.error('Use package_windows.py to build the offline Windows installer')
    ext = ''
    resources = stage_resources(out, args.native_dir, args.build_id, ext)
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
