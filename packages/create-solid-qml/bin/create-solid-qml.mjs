#!/usr/bin/env node
import { scaffold } from "../index.mjs";

const args = process.argv.slice(2);
const tIdx = args.findIndex((a) => a === "--template" || a === "-t");
const template = tIdx >= 0 ? args.splice(tIdx, 2)[1] : undefined;
try {
  await scaffold({ dir: args.find((a) => !a.startsWith("-")), template });
} catch (err) {
  console.error(`create-solid-qml: ${err.message}`);
  process.exitCode = 1;
}
