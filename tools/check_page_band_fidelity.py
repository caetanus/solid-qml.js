#!/usr/bin/env python3
"""Article footer regression: verify fields and compare raster ink boxes to LO.
The fixed crop covers the article's centered footer band, excluding body text.
Every x/y edge must agree within 3 px; missing ink is always a failure.
"""
import json
from pathlib import Path
import sys
from PIL import Image, ImageChops
out = Path(sys.argv[1])
office = json.loads((out/'office.json').read_text())
results = []
for page in range(1, office['pages']+1):
    boxes = {}
    for renderer in ('lo', 'office'):
        crop = Image.open(out/f'{renderer}-{page}.png').convert('L').crop((360,935,456,980))
        box = crop.point(lambda p: 255 if p < 180 else 0).getbbox()
        boxes[renderer] = [box[0]+360,box[1]+935,box[2]+360,box[3]+935] if box else None
    field = any(b['page']==page and b['area']=='footer' and b['text'].strip()==str(page) for b in office.get('bands', []))
    delta = max(abs(a-b) for a,b in zip(boxes['lo'], boxes['office'])) if all(boxes.values()) else None
    results.append(dict(page=page, boxes=boxes, correct_field=field, max_edge_delta_px=delta,
                        passed=field and delta is not None and delta <= 3))
(out/'band-metric.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
sys.exit(0 if all(r['passed'] for r in results) else 1)
