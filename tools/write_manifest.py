#!/usr/bin/env python3
"""Generate an OTA manifest for one ESP32-C3 binary built with PlatformIO."""
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / 'src' / 'main.cpp').read_text(encoding='utf-8')
match = re.search(r'^#define FW_VERSION\s+"(\d+\.\d+\.\d+)"', source, re.M)
if not match:
    raise SystemExit('Expected #define FW_VERSION "MAJOR.MINOR.PATCH" in src/main.cpp')
version = match.group(1)
binary = ROOT / '.pio' / 'build' / 'esp32c3' / 'firmware.bin'
if not binary.is_file():
    raise SystemExit('Missing PlatformIO firmware.bin. Run: pio run -e esp32c3')

out = ROOT / 'build' / 'esp32c3'
out.mkdir(parents=True, exist_ok=True)
blob = binary.read_bytes()
(out / 'firmware.bin').write_bytes(blob)
manifest = {
    'version': version,
    'target': 'esp32c3',
    'size': len(blob),
    'sha256': hashlib.sha256(blob).hexdigest(),
}
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(ROOT / 'version.txt').write_text(version + '\n')
print(f'OTA {version}: {len(blob):,} bytes / SHA256 {manifest["sha256"]}')
