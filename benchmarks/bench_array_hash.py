#!/usr/bin/env python3

# Push 2,000,000 integers into a list
arr = []
for i in range(2_000_000):
    arr.append(i)
print(f"array size: {len(arr)}")

# Sum every 100th element to verify
s = 0
for j in range(0, 2_000_000, 100):
    s += arr[j]
print(f"array checksum: {s}")

# Insert 500,000 key-value pairs into a dict
h = {}
for k in range(500_000):
    h[f"key{k}"] = k
print(f"hash size: {len(h)}")

# Look up all 500,000 values
lookup_sum = 0
for m in range(500_000):
    lookup_sum += h[f"key{m}"]
print(f"lookup sum: {lookup_sum}")

# Delete all keys
for n in range(500_000):
    del h[f"key{n}"]
print(f"after delete: {len(h)}")
