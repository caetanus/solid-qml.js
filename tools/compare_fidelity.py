#!/usr/bin/env python3
"""Compare render-fidelity JSON with pdftotext -bbox-layout (96 dpi coordinates).
Usage: compare_fidelity.py reference.pdf run-directory
Needs pdftotext, pdftoppm and Pillow. All artifacts stay in run-directory.
"""
import difflib
import json
from pathlib import Path
import statistics
import subprocess
import sys
import unicodedata
import xml.etree.ElementTree as ET
from PIL import Image

pdf, out = Path(sys.argv[1]), Path(sys.argv[2])
subprocess.run(['pdftotext', '-bbox-layout', str(pdf), str(out/'lo.html')], check=True)
subprocess.run(['pdftoppm', '-r', '96', '-png', str(pdf), str(out/'lo')], check=True)
root = ET.parse(out/'lo.html')
ns = {'h': 'http://www.w3.org/1999/xhtml'}
reference = []
pages = root.findall('.//h:page', ns)
for p, page in enumerate(pages, 1):
    for b, block in enumerate(page.findall('.//h:block', ns)):
        for line in block.findall('h:line', ns):
            reference.append(dict(page=p, block=f'{p}-{b}', x=float(line.attrib['xMin'])*4/3,
                                  y=float(line.attrib['yMin'])*4/3,
                                  text=' '.join(w.text or '' for w in line.findall('h:word', ns))))
office = json.loads((out/'office.json').read_text())
def norm(s):
    return ' '.join(unicodedata.normalize('NFKC', s).replace('\u00ad', '').split())
a, b = [norm(l['text']) for l in reference], [norm(l['text']) for l in office['lines']]
matcher = difflib.SequenceMatcher(None, a, b, autojunk=False)
pairs = [(i+k,j+k) for i,j,n in matcher.get_matching_blocks() for k in range(n) if a[i+k]]
deltas, seen = {}, set()
for i,j in pairs:
    r,o = reference[i], office['lines'][j]
    key = (r['page'],r['block'],o['block'])
    if key not in seen and r['page']==o['page']:
        deltas.setdefault(r['page'], []).append(o['y']-r['y']); seen.add(key)
paragraphs = []
for block in dict.fromkeys(l['block'] for l in office['lines']):
    ls=[norm(l['text']) for l in office['lines'] if l['block']==block and norm(l['text'])]
    matches=[i for i,j in pairs if office['lines'][j]['block']==block]
    paragraphs.append(dict(block=block, same=bool(ls) and len(matches)==len(ls), matched_lines=len(matches), lines=len(ls)))
metric=dict(office_pages=office['pages'], lo_pages=len(pages), matched_lines=len(pairs),
            office_lines=len(b), lo_lines=len(a), paragraphs=paragraphs,
            block_offsets={p:dict(median_abs_px=statistics.median(map(abs,v)),max_abs_px=max(map(abs,v)),
                                 matched_blocks=len(v), signed_deltas=v) for p,v in deltas.items()},
            note='Offsets use first matched line per block pair on the same page; unmatched lines and page migrations are excluded, not zero error. PDF yMin is glyph bounds; Qt y is line box top.')
(out/'metric.json').write_text(json.dumps(metric,indent=2)+'\n')
(out/'lo.json').write_text(json.dumps(reference,indent=2)+'\n')
for p in range(1,max(len(pages),office['pages'])+1):
    paths=[out/f'lo-{p}.png',out/f'office-{p}.png']
    images=[Image.open(path).convert('RGB') if path.exists() else Image.new('RGB',(816,1056),'#ddd') for path in paths]
    combined=Image.new('RGB',(sum(im.width for im in images),max(im.height for im in images)), '#ddd')
    combined.paste(images[0],(0,0)); combined.paste(images[1],(images[0].width,0))
    combined.save(out/f'side-by-side-{p}.png')
print(json.dumps({k:v for k,v in metric.items() if k!='paragraphs'},indent=2))
