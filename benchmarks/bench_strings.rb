#!/usr/bin/env ruby

# Concatenate "hello" 500,000 times
s = ""
500_000.times { s += "hello" }
puts "concat len: #{s.length}"

# Split a large string 100,000 times
csv = "alpha,bravo,charlie,delta,echo,foxtrot,golf,hotel"
total_parts = 0
100_000.times do
  parts = csv.split(",")
  total_parts += parts.length
end
puts "split parts: #{total_parts}"

# Regex replace on a template string 200,000 times
template = "Hello NAME, welcome to PLACE on DATE"
result = ""
pat_name = /NAME/
pat_place = /PLACE/
pat_date = /DATE/
200_000.times do
  result = template.dup
  result.sub!(pat_name, "World")
  result.sub!(pat_place, "Strada")
  result.sub!(pat_date, "today")
end
puts "regex result: #{result}"
