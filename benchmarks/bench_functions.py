#!/usr/bin/env python3

import sys

sys.setrecursionlimit(100000)

def add3(a, b, c):
    return a + b + c

def ackermann(m, n):
    if m == 0:
        return n + 1
    if n == 0:
        return ackermann(m - 1, 1)
    return ackermann(m - 1, ackermann(m, n - 1))

# Call a simple 3-arg function 5,000,000 times
s = 0
for i in range(5_000_000):
    s += add3(i, i + 1, i + 2)
print(f"call sum: {s}")

# Compute ackermann(3,8)
ack = ackermann(3, 8)
print(f"ackermann(3,8): {ack}")
