s = ""
for i in range(10000): s += "hello"
print(f"concat len: {len(s)}")
total = 0
for j in range(1000):
    parts = "a,b,c,d,e,f,g,h".split(",")
    total += len(parts)
print(f"split parts: {total}")
