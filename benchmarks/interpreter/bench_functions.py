def add(a, b): return a + b
def mul(a, b): return a * b
s = 0
for i in range(100000): s += add(i, mul(i, 2))
print(f"sum: {s}")
