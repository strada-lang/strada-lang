#!/usr/bin/env python3

class Point:
    def __init__(self, x=0, y=0):
        self.x = x
        self.y = y

    def distance_sq(self):
        return self.x * self.x + self.y * self.y

class Point3D(Point):
    def __init__(self, x=0, y=0, z=0):
        super().__init__(x, y)
        self.z = z

    def distance_sq(self):
        return self.x * self.x + self.y * self.y + self.z * self.z

# Create 500,000 Point objects, call method on each
s = 0
for i in range(500_000):
    p = Point(x=i % 100, y=(i + 1) % 100)
    s += p.distance_sq()
print(f"point sum: {s}")

# Create 500,000 Point3D objects (inherited), call overridden method
s3d = 0
for j in range(500_000):
    p = Point3D(x=j % 100, y=(j + 1) % 100, z=(j + 2) % 100)
    s3d += p.distance_sq()
print(f"point3d sum: {s3d}")

# isa() checks on 200,000 objects
isa_count = 0
for k in range(200_000):
    p = Point3D(x=1, y=2, z=3)
    if isinstance(p, Point):
        isa_count += 1
print(f"isa checks: {isa_count}")
