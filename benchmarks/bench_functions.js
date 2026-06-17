#!/usr/bin/env node

function add3(a, b, c) {
    return a + b + c;
}

function ackermann(m, n) {
    if (m === 0) return n + 1;
    if (n === 0) return ackermann(m - 1, 1);
    return ackermann(m - 1, ackermann(m, n - 1));
}

// Call a simple 3-arg function 5,000,000 times
let s = 0;
for (let i = 0; i < 5_000_000; i++) {
    s += add3(i, i + 1, i + 2);
}
console.log(`call sum: ${s}`);

// Compute ackermann(3,8)
const ack = ackermann(3, 8);
console.log(`ackermann(3,8): ${ack}`);
