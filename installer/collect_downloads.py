"""Collect the two standalone downloads; this does not publish them."""
import hashlib
from pathlib import Path
import shutil
ROOT = Path(__file__).resolve().parents[1]
output = ROOT / 'out/installer/downloads'
output.mkdir(parents=True, exist_ok=True)
files = [ROOT / 'out/installer/Glaze-Installer-macos-arm64.dmg',
         ROOT / 'out/installer-windows/Glaze-Installer-windows-x64.exe']
for file in files:
    shutil.copy2(file, output / file.name)
(output / 'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest() + '  ' + f.name + '\n' for f in files))
shutil.copy2(ROOT / 'installer/START-HERE.txt', output)
print(output)
