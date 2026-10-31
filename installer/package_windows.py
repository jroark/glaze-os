"""Build a single offline Windows setup EXE on macOS/Linux with MinGW and NSIS.

No user-installed Python, separate firmware or driver download is required.
Downloads are locked to official upstream URLs and SHA-256 digests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request
import zipfile

from package import ROOT, stage_resources


def run(*command):
    subprocess.run(list(map(str, command)), check=True)


def download_dependencies(cache):
    cache.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / 'installer/windows-downloads.json').read_text())
    for name, entry in lock.items():
        target = cache / name
        if not target.exists():
            partial = cache / (name + '.partial')
            urllib.request.urlretrieve(entry['url'], partial)
            partial.rename(target)
        if hashlib.sha256(target.read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError('Dependency checksum mismatch: ' + name)
    return lock


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-id', required=True)
    args = parser.parse_args()
    cache = ROOT / 'out/installer-downloads'
    download_dependencies(cache)
    out = ROOT / 'out/installer-windows'
    native = out / 'native'
    native.mkdir(parents=True, exist_ok=True)
    libusb = cache / 'libusb'
    # Only extract x64 MinGW files: old p7zip cannot decode ARM64 BCJ filters.
    run('7z', 'x', '-y', '-o' + str(libusb), cache / 'libusb-1.0.30.7z', 'MinGW64/*', 'include/*')
    tcl = cache / 'tcltk'
    run('msiextract', '-C', tcl, cache / 'tcltk.msi')
    cc = 'x86_64-w64-mingw32-gcc'
    windres = 'x86_64-w64-mingw32-windres'
    rc = native / 'installer.rc'
    rc.write_text('1 24 "' + str(ROOT / 'installer/windows.manifest') + '"\n')
    resource = native / 'manifest.o'
    run(windres, '-i', rc, '-o', resource)
    includes = ['installer/compat', 'host/glaze_usb', 'lib/include', 'servers/fs',
                'third_party/littlefs', 'third_party/dhara']
    flags = ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-static',
             '-pthread', '-include', str(ROOT / 'installer/compat.h'), '-I' + str(libusb / 'include')]
    flags += ['-I' + str(ROOT / name) for name in includes]
    def compile(name, sources, libraries=()):
        run(cc, *flags, *[ROOT / p for p in sources], resource, *libraries, '-o', native / name)
    usb = 'host/glaze_usb/'
    compile('glaze-installer-usb.exe', ['installer/helper.c'] + [usb + n + '.c' for n in
            ('transport', 'client', 'pcconnect', 'nand', 'nand_write')],
            [libusb / 'MinGW64/static/libusb-1.0.dll.a', '-lbcrypt'])
    compile('nand-inventory.exe', ['host/nand_inventory.c', 'kernel/drivers/nand_boot.c', 'kernel/drivers/nand_ecc.c'])
    fs = ['host/mkfsimg.c', 'servers/fs/ftl.c', 'third_party/littlefs/lfs.c', 'third_party/littlefs/lfs_util.c']
    fs += [str(p.relative_to(ROOT)) for p in (ROOT / 'third_party/dhara/dhara').glob('*.c')]
    fs += [str(p.relative_to(ROOT)) for p in (ROOT / 'third_party/dhara/ecc').glob('*.c')]
    compile('mkfsimg.exe', fs)
    product = out / 'Glaze Installer'
    if product.exists():
        shutil.rmtree(product)
    product.mkdir()
    resources = stage_resources(product, native, args.build_id, '.exe')
    shutil.copy2(libusb / 'MinGW64/dll/libusb-1.0.dll', resources / 'bin')
    (resources / 'drivers').mkdir()
    shutil.copy2(cache / 'zadig-2.9.exe', resources / 'drivers')
    for name in ('libusb-1.0.30.tar.bz2', 'libwdi-v1.5.1.zip'):
        shutil.copy2(cache / name, resources / 'licenses')
    shutil.copy2(ROOT / 'installer/windows-downloads.json', resources / 'licenses')
    manifest_path = resources / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    for folder in ('drivers', 'licenses'):
        for path in (resources / folder).iterdir():
            manifest['files'][path.relative_to(resources).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    manifest_path.write_text(json.dumps(manifest, indent=2))
    runtime = product / 'runtime'
    with zipfile.ZipFile(cache / 'python-3.11.9-embed-amd64.zip') as archive:
        archive.extractall(runtime)
    shutil.copytree(tcl / 'DLLs', runtime / 'DLLs')
    shutil.copytree(tcl / 'Lib/tkinter', runtime / 'Lib/tkinter')
    shutil.copytree(tcl / 'tcl', runtime / 'tcl')
    (runtime / 'python311._pth').write_text('python311.zip\n.\nDLLs\nLib\n../app\n')
    app = product / 'app'
    app.mkdir()
    for name in ('app.py', 'engine.py', 'app_selection.py'):
        shutil.copy2(ROOT / 'installer' / name, app)
    run(cc, '-O2', '-static', '-municode', '-mwindows', ROOT / 'installer/windows-launcher.c', resource,
        '-o', product / 'Glaze Installer.exe')
    shutil.copy2(ROOT / 'installer/START-HERE.txt', product)
    output = out / 'Glaze-Installer-windows-x64.exe'
    run('makensis', '-DPRODUCT_DIR=' + str(product), '-DOUTPUT_FILE=' + str(output), ROOT / 'installer/windows.nsi')
    (out / 'SHA256SUMS').write_text(hashlib.sha256(output.read_bytes()).hexdigest() + '  ' + output.name + '\n')
    print('Created', output)


if __name__ == '__main__':
    main()
