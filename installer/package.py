"""Stage a matching build and package it on the target host. Never accesses USB."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build-id', required=True, help='BUILD_ID used to compile the bundled kernel')
    p.add_argument('--native-dir', type=Path, default=ROOT / 'out/installer-native')
    p.add_argument('--stage-only', action='store_true')
    args = p.parse_args()
    out = ROOT / 'out/installer'
    resources = out / 'resources'
    if resources.exists():
        shutil.rmtree(resources)
    (resources / 'bin').mkdir(parents=True)
    ext = '.exe' if sys.platform == 'win32' else ''
    for name in ('glaze-installer-usb', 'nand-inventory', 'mkfsimg'):
        shutil.copy2(args.native_dir / (name + ext), resources / 'bin')
    if ext:
        # MSYS2 MinGW DLL dependencies; PyInstaller also scans these binaries.
        for dll in Path(shutil.which('gcc')).parent.glob('*.dll'):
            if dll.name.lower().startswith(('libusb-1.0', 'libwinpthread', 'libgcc_s')):
                shutil.copy2(dll, resources / 'bin')
    (resources / 'ram').mkdir()
    for name in ('GlazeBoot.exe', 'UsbProbe.exe', 'kernel.bin', 'romfs.img'):
        shutil.copy2(ROOT / 'out/glazeboot' / name, resources / 'ram')
    shutil.copy2(ROOT / 'out/glaze.bin', resources)
    shutil.copy2(ROOT / 'LICENSE', resources / 'LICENSE')
    shutil.copytree(ROOT / 'userdata', resources / 'userdata')
    (resources / 'tools').mkdir()
    for name in ('physical_nand.py', 'nand_transaction.py'):
        shutil.copy2(ROOT / 'tools' / name, resources / 'tools')
    notices = resources / 'licenses'
    notices.mkdir()
    for component in ('littlefs', 'dhara'):
        for path in (ROOT / 'third_party' / component).glob('*'):
            if path.is_file() and path.name.lower().startswith(('license', 'copying')):
                shutil.copy2(path, notices / (component + '-' + path.name))
    manifest = {'version': 1, 'build_id': args.build_id, 'files': {}}
    for path in sorted(resources.rglob('*')):
        if path.is_file() and path.parent != resources / 'bin':
            manifest['files'][str(path.relative_to(resources)).replace('\\', '/')] = hashlib.sha256(path.read_bytes()).hexdigest()
    (resources / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    if args.stage_only:
        return
    sep = ';' if ext else ':'
    command = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--name', 'Glaze Installer',
               '--onedir', '--distpath', str(out / 'dist'), '--workpath', str(out / 'build'),
               '--specpath', str(out), '--add-data', str(resources) + sep + 'resources',
               '--hidden-import', 'tkinter', '--hidden-import', 'tkinter.ttk',
               '--hidden-import', 'tkinter.filedialog', '--hidden-import', 'tkinter.messagebox']
    for path in (resources / 'bin').iterdir():
        command += ['--add-binary', str(path) + sep + 'resources/bin']
    # Console mode is required on Windows for worker stdout pipes; the GUI
    # launches workers hidden. The setup launcher itself uses pythonw via a
    # separate windowed build in a future public release.
    if not ext:
        command += ['--windowed', '--osx-bundle-identifier', 'com.glazeos.installer']
    command += [str(ROOT / 'installer/app.py')]
    subprocess.run(command, check=True)
    arch = platform.machine().lower()
    if ext:
        subprocess.run(['iscc', '/O' + str(out), str(ROOT / 'installer/windows.iss')], check=True)
    else:
        subprocess.run(['hdiutil', 'create', '-volname', 'Glaze Installer', '-srcfolder',
                        str(out / 'dist/Glaze Installer.app'), '-ov', str(out / f'Glaze-Installer-macos-{arch}.dmg')], check=True)
    paths = list(out.glob('*.dmg')) + list(out.glob('*.exe'))
    (out / 'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest() + '  ' + f.name + '\n' for f in paths))


if __name__ == '__main__':
    main()
