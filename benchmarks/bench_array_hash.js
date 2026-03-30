#!/usr/bin/env node

// Push 2,000,000 integers into an array
const arr = [];
for (let i = 0; i < 2_000_000; i++) {
    arr.push(i);
}
console.log(`array size: ${arr.length}`);

// Sum every 100th element to verify
let s = 0;
for (let j = 0; j < 2_000_000; j += 100) {
    s += arr[j];
}
console.log(`array checksum: ${s}`);

// Insert 500,000 key-value pairs into a Map
const h = new Map();
for (let k = 0; k < 500_000; k++) {
    h.set(`key${k}`, k);
}
console.log(`hash size: ${h.size}`);

// Look up all 500,000 values
let lookup_sum = 0;
for (let m = 0; m < 500_000; m++) {
    lookup_sum += h.get(`key${m}`);
}
console.log(`lookup sum: ${lookup_sum}`);

// Delete all keys
for (let n = 0; n < 500_000; n++) {
    h.delete(`key${n}`);
}
console.log(`after delete: ${h.size}`);
