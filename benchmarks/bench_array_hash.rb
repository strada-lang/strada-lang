#!/usr/bin/env ruby

# Push 2,000,000 integers into an array
arr = []
2_000_000.times { |i| arr.push(i) }
puts "array size: #{arr.length}"

# Sum every 100th element to verify
s = 0
(0...2_000_000).step(100) { |j| s += arr[j] }
puts "array checksum: #{s}"

# Insert 500,000 key-value pairs into a hash
h = {}
500_000.times { |k| h["key#{k}"] = k }
puts "hash size: #{h.length}"

# Look up all 500,000 values
lookup_sum = 0
500_000.times { |m| lookup_sum += h["key#{m}"] }
puts "lookup sum: #{lookup_sum}"

# Delete all keys
500_000.times { |n| h.delete("key#{n}") }
puts "after delete: #{h.length}"
