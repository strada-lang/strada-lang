#!/usr/bin/env node
// Node counterpart of bench_utf8.strada (JS strings are UTF-16
// codepoints; byte lengths via Buffer for parity of reported work).
const corpus = [];
for (let i = 0; i < 200000; i++)
    corpus.push(`Müller-Straße ${i} — café №${i % 100} übergröße`);
const raw = corpus.map(s => Buffer.from(s, "utf8"));
const dec = new TextDecoder("utf-8", { fatal: true });

let t0 = Date.now();
let caseLen = 0;
for (const s of corpus) { const u = s.toUpperCase(); const l = u.toLowerCase(); caseLen += Buffer.byteLength(l); }
let t1 = Date.now();
console.log("case:", caseLen, (t1 - t0) / 1000);

let valid = 0;
for (const b of raw) { try { dec.decode(b); valid++; } catch (e) {} }
let t2 = Date.now();
console.log("valid:", valid, (t2 - t1) / 1000);

let built = 0;
for (let i = 0; i < 50000; i++) {
    let s = "";
    for (let j = 0; j < 20; j++) {
        s += String.fromCodePoint(0xE9 + (j % 16));
        s += String.fromCodePoint(0x4E00 + (j % 64));
    }
    built += Buffer.byteLength(s);
}
let t3 = Date.now();
console.log("chr-build:", built, (t3 - t2) / 1000);

let acc = "";
for (let i = 0; i < 30000; i++) acc += "ü" + i + "ß—";
let t4 = Date.now();
console.log("concat:", Buffer.byteLength(acc), (t4 - t3) / 1000);

const decomposed = "Café resumé naivë";
let nfcLen = 0;
for (let i = 0; i < 50000; i++) nfcLen += Buffer.byteLength(decomposed.normalize("NFC"));
let t5 = Date.now();
console.log("nfc:", nfcLen, (t5 - t4) / 1000);
console.log("total:", (t5 - t0) / 1000);
