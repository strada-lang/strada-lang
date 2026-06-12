#!/usr/bin/env node
// Node counterpart of bench_pipeline.strada (identical workload).
let t0 = Date.now();
const squares = Array.from({length: 2000000}, (_, i) => (i + 1) * (i + 1));
let t1 = Date.now();
console.log("map-range:", squares[1999999], (t1 - t0) / 1000);

const mults = [];
for (let i = 1; i <= 2000000; i++) if (i % 7 === 0) mults.push(i);
let t2 = Date.now();
console.log("grep-range:", mults.length, (t2 - t1) / 1000);

const base = []; let seed = 42;
for (let i = 0; i < 300000; i++) { seed = (seed * 1103515245 + 12345) % 2147483648; base.push(seed % 1000000); }
const mapped = base.map(x => x * 3 + 1);
const kept = mapped.filter(x => x % 2 === 1);
const ordered = kept.sort((a, b) => b - a);
let top = 0; for (let i = 0; i < 100; i++) top += ordered[i];
let t3 = Date.now();
console.log("chain:", top, (t3 - t2) / 1000);

const words = []; for (let i = 0; i < 500000; i++) words.push("w" + (i % 1000));
let jlen = 0;
for (let r = 0; r < 3; r++) jlen += words.join(",").length;
let t4 = Date.now();
console.log("join:", jlen, (t4 - t3) / 1000);

const csv = words.join(",");
let slen = 0;
for (let r = 0; r < 3; r++) slen += csv.split(",").length;
let t5 = Date.now();
console.log("split-join:", slen, (t5 - t4) / 1000);
console.log("total:", (t5 - t0) / 1000);
