#!/usr/bin/env ruby

def add3(a, b, c)
  a + b + c
end

def ackermann(m, n)
  if m == 0
    return n + 1
  end
  if n == 0
    return ackermann(m - 1, 1)
  end
  ackermann(m - 1, ackermann(m, n - 1))
end

# Call a simple 3-arg function 5,000,000 times
s = 0
5_000_000.times do |i|
  s += add3(i, i + 1, i + 2)
end
puts "call sum: #{s}"

# Compute ackermann(3,8)
ack = ackermann(3, 8)
puts "ackermann(3,8): #{ack}"
