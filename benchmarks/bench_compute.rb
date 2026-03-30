#!/usr/bin/env ruby

def fib(n)
  return n if n < 2
  fib(n - 1) + fib(n - 2)
end

# Sum integers 1 to 50,000,000
s = 0
(1..50_000_000).each { |i| s += i }
puts "sum: #{s}"

# Recursive fibonacci(35), run 30 times
fib_result = 0
30.times { fib_result = fib(35) }
puts "fib(35): #{fib_result}"
