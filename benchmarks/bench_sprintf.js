#!/usr/bin/env node
// Node counterpart of bench_sprintf.strada. JS has no sprintf; the
// idiomatic equivalents (template literals, toFixed/toString(16),
// padStart/padEnd) produce the same output strings.
let t0 = Date.now();
let mlen = 0;
for (let i = 0; i < 500000; i++) {
    const line = `[INFO] user=u${i % 1000} req=${i} bytes=${(i * 37) % 100000} t=${((i % 500) / 7.0).toFixed(2)}ms`;
    mlen += line.length;
}
let t1 = Date.now();
console.log("mixed:", mlen, (t1 - t0) / 1000);

let nlen = 0;
for (let i = 0; i < 1000000; i++) nlen += `${i} ${i.toString(16)} ${i.toString(8)}`.length;
let t2 = Date.now();
console.log("numeric:", nlen, (t2 - t1) / 1000);

let flen = 0;
for (let i = 0; i < 500000; i++) {
    const v = i / 3.0;
    flen += `${v.toFixed(6)} ${v.toExponential(6)} ${String(v)}`.length;
}
let t3 = Date.now();
console.log("float:", flen, (t3 - t2) / 1000);

let wlen = 0;
for (let i = 0; i < 500000; i++) {
    const row = `${("row_" + (i % 100)).padEnd(20)}|${String(i).padStart(10)}|${String(i % 1000).padStart(8, "0")}|${((i % 1000) / 10.0).toFixed(1).padStart(6)}%`;
    wlen += row.length;
}
let t4 = Date.now();
console.log("width:", wlen, (t4 - t3) / 1000);
console.log("total:", (t4 - t0) / 1000);
