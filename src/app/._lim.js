import { readFileSync, statSync } from "node:fs";
import { Capture } from "./src/mcp/capture_store.js";
import { parseHwCounters, LIMITER_LABEL, limiterAdvice } from "./src/renderer/hw_counters.js";
const [cap, json] = process.argv.slice(2);
const c = new Capture("t", cap, statSync(cap).mtimeMs, new Uint8Array(readFileSync(cap)));
c.data.hwCounters = parseHwCounters(readFileSync(json, "utf8"));
for (const p of c.metrics.passes.filter((x) => !x.compute && x.limiter)) {
  const l = p.limiter;
  console.log(`${p.label}: ${LIMITER_LABEL[l.kind]} (${l.label} ${l.percent.toFixed(1)}%)`);
  console.log(`  registers: ${l.registers ? `${l.registers.stage} ${l.registers.count}` : "(none attached)"}`);
  if (l.kind === "occupancy") console.log(`  advice: ${limiterAdvice(l).slice(0, 220)}`);
}
