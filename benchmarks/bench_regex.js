#!/usr/bin/env node
// Node counterpart of bench_regex.strada (identical workload; tr/// is
// emulated with replace+callback, the closest JS equivalent).
const lines = [];
for (let i = 0; i < 200000; i++) {
    const code = 200 + ((i * 7) % 4) * 100;
    const bytes = (i * 37) % 100000;
    lines.push(`10.0.${Math.floor(i/256)%256}.${i%256} - - [12/Jun/2026:01:0${i%10}:00] "GET /page/${i%1000} HTTP/1.1" ${code} ${bytes}`);
}

let t0 = Date.now();
let hits = 0;
const reMatch = /" (4|5)\d\d /;
for (const l of lines) if (reMatch.test(l)) hits++;
let t1 = Date.now();
console.log("match:", hits, (t1 - t0) / 1000);

let bytesTotal = 0;
const reCap = /^(\S+) .* "(\w+) [^"]*" (\d+) (\d+)$/;
for (const l of lines) { const m = reCap.exec(l); if (m) bytesTotal += +m[4]; }
let t2 = Date.now();
console.log("captures:", bytesTotal, (t2 - t1) / 1000);

let codeSum = 0;
const reNamed = /" (?<code>\d+) (?<bytes>\d+)$/;
for (let i = 0; i < 100000; i++) { const m = reNamed.exec(lines[i]); if (m) codeSum += +m.groups.code; }
let t3 = Date.now();
console.log("named:", codeSum, (t3 - t2) / 1000);

let subLen = 0;
for (let i = 0; i < 100000; i++) subLen += lines[i].replace(/\d+/g, "N").length;
let t4 = Date.now();
console.log("subst:", subLen, (t4 - t3) / 1000);

let subeLen = 0;
for (let i = 0; i < 50000; i++) subeLen += lines[i].replace(/(\d+)/g, (m) => String(m.length)).length;
let t5 = Date.now();
console.log("subst-e:", subeLen, (t5 - t4) / 1000);

let trCount = 0;
for (let i = 0; i < 100000; i++) {
    let c = 0;
    lines[i].replace(/[a-z]/g, (m) => { c++; return m.toUpperCase(); });
    trCount += c;
}
let t6 = Date.now();
console.log("tr:", trCount, (t6 - t5) / 1000);
console.log("total:", (t6 - t0) / 1000);
