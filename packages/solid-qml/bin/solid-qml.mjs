#!/usr/bin/env node
import { main } from "../lib/cli.mjs";

const code = await main(process.argv.slice(2));
if (code !== null) process.exitCode = code;
