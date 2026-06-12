#!/usr/bin/env node
// Node counterpart of bench_sort.strada (identical workload).
let seed = 42;
function lcg() { seed = (seed * 1103515245 + 12345) % 2147483648; return seed; }

const ints = []; for (let i = 0; i < 1000000; i++) ints.push(lcg() % 10000000);
const strs = []; for (let i = 0; i < 500000; i++) strs.push("key_" + (lcg() % 1000000) + "_suffix");

let t0 = Date.now();
const si = [...ints].sort((a, b) => a - b);
let t1 = Date.now();
console.log("int-sort:", si[0], (t1 - t0) / 1000);

const ss = [...strs].sort();
let t2 = Date.now();
console.log("str-sort:", ss[0].length, (t2 - t1) / 1000);

const half = ints.slice(0, 500000);
const sc = half.sort((a, b) => b - a);
let t3 = Date.now();
console.log("cmp-sort:", sc[0], (t3 - t2) / 1000);

const h = {}; for (let i = 0; i < 200000; i++) h["k" + i] = i;
let hsum = 0;
for (let r = 0; r < 5; r++) { const hk = Object.keys(h).sort(); hsum += hk[0].length; }
let t4 = Date.now();
console.log("hash-sort:", hsum, (t4 - t3) / 1000);
console.log("total:", (t4 - t0) / 1000);
