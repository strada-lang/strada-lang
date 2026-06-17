arr = []
for i in range(50000): arr.append(i)
print(f"array size: {len(arr)}")
h = {}
for k in range(10000): h[f"key{k}"] = k
print(f"hash size: {len(h)}")
