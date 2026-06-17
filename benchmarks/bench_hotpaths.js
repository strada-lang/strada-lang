// Node equivalent of bench_hotpaths.strada — same workloads, same counts.
"use strict";

class Greeter {
    constructor(n) { this.n = n; }
    bump() { return this.n + 1; }
}

function hr() { return process.hrtime.bigint(); }
function secs(a, b) { return Number(b - a) / 1e9; }

function main() {
    let t0 = hr();

    // 1. dynamic dispatch: receiver fetched from an array. (V8 will still
    // inline-cache this — that's the honest comparison point.)
    const objs = [new Greeter(5)];
    let sum = 0;
    for (let i = 0; i < 5000000; i++) {
        const p = objs[0];
        sum += p.bump();
    }
    let t1 = hr();
    console.log(`dispatch: ${sum} ${secs(t0, t1)}`);

    // 2. hash counter loop with computed keys
    const c = {};
    for (let j = 0; j < 10000000; j++) {
        const k = "key" + (j % 100);
        c[k] = (c[k] || 0) + 1;
    }
    let t2 = hr();
    console.log(`hash: ${c["key0"]} ${secs(t1, t2)}`);

    // 3. range loop
    let rsum = 0;
    for (let r = 0; r <= 20000000; r++) {
        rsum += r;
    }
    let t3 = hr();
    console.log(`range: ${rsum} ${secs(t2, t3)}`);

    // 4. large-string concat. The tail alternates per iteration and a
    // mid-string char is read so V8 can neither hoist the concat as
    // loop-invariant nor leave the result as an unmaterialized
    // cons-string — the copy being measured actually happens
    // (matches the Strada version).
    const big = "y".repeat(100000);
    const tails = ["z".repeat(1000), "w".repeat(1000)];
    let clen = 0;
    for (let m = 0; m < 20000; m++) {
        const t = big + tails[m % 2];
        clen += t.length + t.charCodeAt(50000);
    }
    let t4 = hr();
    console.log(`concat: ${clen} ${secs(t3, t4)}`);

    // 5. object construction + accessor
    let obj = 0;
    for (let o = 0; o < 2000000; o++) {
        const p = new Greeter(o % 100);
        obj += p.n;
    }
    let t5 = hr();
    console.log(`objects: ${obj} ${secs(t4, t5)}`);
}
main();
