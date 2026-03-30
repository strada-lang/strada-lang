#!/usr/bin/env node

function fib(n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

// Sum integers 1 to 50,000,000
let s = 0;
for (let i = 1; i <= 50_000_000; i++) {
    s += i;
}
console.log(`sum: ${s}`);

// Recursive fibonacci(35), run 30 times
let fib_result = 0;
for (let j = 0; j < 30; j++) {
    fib_result = fib(35);
}
console.log(`fib(35): ${fib_result}`);
