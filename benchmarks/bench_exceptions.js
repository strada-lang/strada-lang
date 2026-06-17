#!/usr/bin/env node
// Node counterpart of bench_exceptions.strada.
class MiscErr extends Error {}
class WantedErr extends Error {}

let t0 = Date.now();
let ok = 0;
for (let i = 0; i < 2000000; i++) { try { ok++; } catch (e) { ok--; } }
let t1 = Date.now();
console.log("try-nothrow:", ok, (t1 - t0) / 1000);

let caught = 0;
for (let i = 0; i < 200000; i++) { try { throw new Error("boom " + i); } catch (e) { caught++; } }
let t2 = Date.now();
console.log("throw-catch:", caught, (t2 - t1) / 1000);

let typed = 0;
for (let i = 0; i < 200000; i++) {
    try { throw new WantedErr(); }
    catch (e) {
        if (e instanceof MiscErr) typed--;
        else if (e instanceof WantedErr) typed++;
        else typed--;
    }
}
let t3 = Date.now();
console.log("typed:", typed, (t3 - t2) / 1000);

function deep(n) { if (n <= 0) throw new Error("bottom"); return deep(n - 1) + 1; }
let deepCaught = 0;
for (let i = 0; i < 20000; i++) { try { deep(50); } catch (e) { deepCaught++; } }
let t4 = Date.now();
console.log("deep-unwind:", deepCaught, (t4 - t3) / 1000);

let fin = 0;
for (let i = 0; i < 1000000; i++) { try { fin++; } finally { fin++; } }
let t5 = Date.now();
console.log("finally:", fin, (t5 - t4) / 1000);
console.log("total:", (t5 - t0) / 1000);
