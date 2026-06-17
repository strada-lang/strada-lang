#!/usr/bin/env ruby

class Point
  attr_accessor :x, :y

  def initialize(x: 0, y: 0)
    @x = x
    @y = y
  end

  def distance_sq
    @x * @x + @y * @y
  end
end

class Point3D < Point
  attr_accessor :z

  def initialize(x: 0, y: 0, z: 0)
    super(x: x, y: y)
    @z = z
  end

  def distance_sq
    @x * @x + @y * @y + @z * @z
  end
end

# Create 500,000 Point objects, call method on each
s = 0
500_000.times do |i|
  p = Point.new(x: i % 100, y: (i + 1) % 100)
  s += p.distance_sq
end
puts "point sum: #{s}"

# Create 500,000 Point3D objects (inherited), call overridden method
s3d = 0
500_000.times do |j|
  p = Point3D.new(x: j % 100, y: (j + 1) % 100, z: (j + 2) % 100)
  s3d += p.distance_sq
end
puts "point3d sum: #{s3d}"

# isa() checks on 200,000 objects
isa_count = 0
200_000.times do
  p = Point3D.new(x: 1, y: 2, z: 3)
  isa_count += 1 if p.is_a?(Point)
end
puts "isa checks: #{isa_count}"
