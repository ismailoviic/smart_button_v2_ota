#!/usr/bin/env python3
"""Keep Arduino IDE sketch and PlatformIO source identical.

By default src/main.cpp -> Arduino sketch.
Use --from-arduino to copy Arduino IDE edits to src/main.cpp.
"""
from pathlib import Path
import sys
root = Path(__file__).resolve().parent.parent
source = root / 'src' / 'main.cpp'
sketch = root / 'arduino' / 'Smart_Button_V2' / 'Smart_Button_V2.ino'
if '--from-arduino' in sys.argv:
    source.write_bytes(sketch.read_bytes())
    print('Synced Arduino IDE sketch -> src/main.cpp')
else:
    sketch.write_bytes(source.read_bytes())
    print('Synced src/main.cpp -> Arduino IDE sketch')
for name in ('TLSRoots.h', 'SB2Types.h'):
    (root / 'arduino' / 'Smart_Button_V2' / name).write_bytes((root / 'src' / name).read_bytes())
