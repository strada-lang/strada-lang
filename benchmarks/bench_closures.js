#!/usr/bin/env node
// Node counterpart of bench_closures.strada (identical workload).
function makeAdder(base) { const bias = base * 2; return (x) => x + bias; }
function makeOuter(seed) {
    const level0 = seed;
    const mid = () => (x) => x + level0;
    return mid();
}

let t0 = Date.now();
let made = 0;
for (let i = 0; i < 500000; i++) { const f = makeAdder(i); made++; }
let t1 = Date.now();
console.log("create:", made, (t1 - t0) / 1000);

const add7 = makeAdder(7);
let sum = 0;
for (let i = 0; i < 5000000; i++) sum += add7(i % 100);
let t2 = Date.now();
console.log("invoke:", sum, (t2 - t1) / 1000);

let counter = 0;
const bump = () => ++counter;
for (let i = 0; i < 2000000; i++) bump();
let t3 = Date.now();
console.log("capture-rw:", counter, (t3 - t2) / 1000);

const table = {};
for (let i = 0; i < 16; i++) table["op" + i] = makeAdder(i);
let tsum = 0;
for (let i = 0; i < 200000; i++) tsum += table["op" + (i % 16)](i % 50);
let t4 = Date.now();
console.log("table:", tsum, (t4 - t3) / 1000);

const deep = makeOuter(11);
let dsum = 0;
for (let i = 0; i < 200000; i++) dsum += deep(i % 10);
let t5 = Date.now();
console.log("transitive:", dsum, (t5 - t4) / 1000);
console.log("total:", (t5 - t0) / 1000);
