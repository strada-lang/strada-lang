#!/usr/bin/env node
// Node counterpart of bench_binary_trees.strada (identical workload).
function build(d) { return d === 0 ? { l: null, r: null } : { l: build(d - 1), r: build(d - 1) }; }
function check(n) { return n.l === null ? 1 : 1 + check(n.l) + check(n.r); }

const maxDepth = 16;
let t0 = Date.now();
let stretch = build(maxDepth + 1);
const sc = check(stretch);
stretch = null;
let t1 = Date.now();
console.log("stretch:", sc, (t1 - t0) / 1000);

const longLived = build(maxDepth);
let t2 = Date.now();
console.log("long-lived: built", (t2 - t1) / 1000);

for (let depth = 4; depth <= 14; depth += 2) {
    const iters = 1 << (maxDepth - depth + 2);
    let sum = 0;
    for (let i = 0; i < iters; i++) sum += check(build(depth));
    console.log(`depth ${depth}: ${iters} trees, check ${sum}`);
}
let t3 = Date.now();
console.log("iterate:", check(longLived), (t3 - t2) / 1000);
console.log("total:", (t3 - t0) / 1000);
