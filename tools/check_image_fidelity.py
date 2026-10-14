#!/usr/bin/env python3
"""Article image-region pixel regression. Run after compare_fidelity.py.
Measure mean squared adjacent-pixel differences after common 2x Lanczos reduction.
20% relative tolerance allows differing subpixel placement and resampling kernels;
it rejects the aliased mail screenshot in the original run-13.
"""
import json
from pathlib import Path
import sys
import numpy as np
from PIL import Image

out = Path(sys.argv[1])
results = {}
for name, box in [('terminal', (96,125,720,455)), ('mail', (96,490,720,880))]:
    energies = {}
    for renderer in ('office', 'lo'):
        crop = Image.open(out / f'{renderer}-3.png').convert('L').crop(box)
        pixels = np.asarray(crop.resize((312, crop.height//2), Image.Resampling.LANCZOS), dtype=float)
        energies[renderer] = float((np.mean(np.diff(pixels,axis=0)**2) + np.mean(np.diff(pixels,axis=1)**2))/2)
    error = abs(energies['office']/energies['lo'] - 1)
    results[name] = dict(energies, relative_error=error, tolerance=0.20, passed=error <= .20)
(out / 'image-metric.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
sys.exit(0 if all(r['passed'] for r in results.values()) else 1)
