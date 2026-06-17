#!/usr/bin/env python3

import sys

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

# Increase recursion limit for safety
sys.setrecursionlimit(10000)

# Sum integers 1 to 50,000,000
s = 0
for i in range(1, 50_000_001):
    s += i
print(f"sum: {s}")

# Recursive fibonacci(35), run 30 times
fib_result = 0
for j in range(30):
    fib_result = fib(35)
print(f"fib(35): {fib_result}")
