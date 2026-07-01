import fs from "node:fs";

// Bidirectional reader for the Source Map v3 the transpiler emits (line-level). Used by the
// debug adapter to translate breakpoints/stops between main.tsx and App.generated.qml.
// All line/column values are 0-based (source-map space); the DAP layer adjusts ±1 as needed.

const B64 = {};
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  .split("")
  .forEach((c, i) => (B64[c] = i));

// Decode a run of base64-VLQ integers.
function decodeVlqs(segment) {
  const out = [];
  let shift = 0;
  let value = 0;
  for (const ch of segment) {
    const d = B64[ch];
    if (d === undefined) continue;
    value += (d & 31) << shift;
    if (d & 32) {
      shift += 5;
    } else {
      const negate = value & 1;
      value >>= 1;
      out.push(negate ? -value : value);
      value = 0;
      shift = 0;
    }
  }
  return out;
}

export function loadSourceMap(mapPath) {
  const map = JSON.parse(fs.readFileSync(mapPath, "utf8"));
  const groups = map.mappings.split(";");

  // Per generated line (0-based): { srcLine, srcCol } or null. Deltas for srcIdx/srcLine/
  // srcCol accumulate across mapped segments (genCol resets per line, irrelevant here).
  const genToSrc = [];
  let srcLine = 0;
  let srcCol = 0;
  for (const group of groups) {
    if (!group) {
      genToSrc.push(null);
      continue;
    }
    const fields = decodeVlqs(group.split(",")[0]); // line-level: first segment only
    if (fields.length >= 4) {
      srcLine += fields[2];
      srcCol += fields[3];
      genToSrc.push({ srcLine, srcCol });
    } else {
      genToSrc.push(null);
    }
  }

  // Source line → first generated line that maps to it.
  const srcToGen = new Map();
  genToSrc.forEach((m, genLine) => {
    if (m && !srcToGen.has(m.srcLine)) srcToGen.set(m.srcLine, genLine);
  });

  return {
    file: map.file,
    sources: map.sources,

    // Generated line (0-based) → source position (0-based) or null.
    generatedToSource(genLine) {
      return genToSrc[genLine] ?? null;
    },

    // Source line (0-based) → generated line (0-based), snapping forward to the next mapped
    // source line if the exact line carries no mapping (so a breakpoint on a blank/comment
    // line lands on the next real statement). null if nothing at/after it is mapped.
    sourceToGenerated(srcLine0) {
      if (srcToGen.has(srcLine0)) return srcToGen.get(srcLine0);
      let best = null;
      let bestLine = Infinity;
      for (const [s, g] of srcToGen) {
        if (s >= srcLine0 && s < bestLine) {
          bestLine = s;
          best = g;
        }
      }
      return best;
    },
  };
}
