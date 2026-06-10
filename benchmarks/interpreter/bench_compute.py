def fib(n):
    if n < 2: return n
    return fib(n-1) + fib(n-2)
s = 0
for i in range(1, 100001): s += i
print(f"sum: {s}")
print(f"fib(25): {fib(25)}")
