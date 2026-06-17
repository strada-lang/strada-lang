#!/usr/bin/env node
// Node counterpart of bench_binary.strada (Buffer-based; pack/unpack
// emulated with explicit writes — the idiomatic JS equivalent).
let t0 = Date.now();
const packed = [];
for (let i = 0; i < 200000; i++) {
    const b = Buffer.alloc(7 + 8);
    b.writeUInt32BE((i * 13) >>> 0, 0);
    b.writeUInt16BE(i % 65536, 4);
    b.writeUInt8(i % 256, 6);
    b.write("payload!", 7);
    packed.push(b);
}
let t1 = Date.now();
console.log("pack:", packed.length, (t1 - t0) / 1000);

let usum = 0;
for (const b of packed) usum += b.readUInt16BE(4);
let t2 = Date.now();
console.log("unpack:", usum, (t2 - t1) / 1000);

const blob = Buffer.alloc(262144);
for (let i = 0; i < 65536; i++) {
    blob[i*4] = i % 256; blob[i*4+1] = (i*7) % 256; blob[i*4+2] = (i*13) % 256; blob[i*4+3] = (i*31) % 256;
}
let b64Len = 0;
for (let r = 0; r < 20; r++) {
    const enc = blob.toString("base64");
    const dec = Buffer.from(enc, "base64");
    b64Len += dec.length;
}
let t3 = Date.now();
console.log("base64:", b64Len, (t3 - t2) / 1000);

const big = Buffer.concat([blob, blob, blob, blob, blob, blob, blob, blob]);
let cksum = 0;
for (let i = 0; i < big.length; i += 16) cksum = (cksum + big[i]) % 65536;
let t4 = Date.now();
console.log("bytes:", cksum, (t4 - t3) / 1000);

const frame = Buffer.alloc(1000000);
for (let i = 0; i < 1000000; i++) frame[i] = i % 256;
let t5 = Date.now();
console.log("build:", frame.length, (t5 - t4) / 1000);
console.log("total:", (t5 - t0) / 1000);
