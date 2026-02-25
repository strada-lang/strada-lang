#!/usr/bin/env python3

import re

# Concatenate "hello" 500,000 times
s = ""
for i in range(500_000):
    s += "hello"
print(f"concat len: {len(s)}")

# Split a large string 100,000 times
csv = "alpha,bravo,charlie,delta,echo,foxtrot,golf,hotel"
total_parts = 0
for j in range(100_000):
    parts = csv.split(",")
    total_parts += len(parts)
print(f"split parts: {total_parts}")

# Regex replace on a template string 200,000 times
template = "Hello NAME, welcome to PLACE on DATE"
result = ""
pat_name = re.compile(r"NAME")
pat_place = re.compile(r"PLACE")
pat_date = re.compile(r"DATE")
for m in range(200_000):
    result = template
    result = pat_name.sub("World", result, count=1)
    result = pat_place.sub("Strada", result, count=1)
    result = pat_date.sub("today", result, count=1)
print(f"regex result: {result}")
