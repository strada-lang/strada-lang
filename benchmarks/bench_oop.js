#!/usr/bin/env node

class Point {
    constructor(x = 0, y = 0) {
        this.x = x;
        this.y = y;
    }

    distance_sq() {
        return this.x * this.x + this.y * this.y;
    }
}

class Point3D extends Point {
    constructor(x = 0, y = 0, z = 0) {
        super(x, y);
        this.z = z;
    }

    distance_sq() {
        return this.x * this.x + this.y * this.y + this.z * this.z;
    }
}

// Create 500,000 Point objects, call method on each
let s = 0;
for (let i = 0; i < 500_000; i++) {
    const p = new Point(i % 100, (i + 1) % 100);
    s += p.distance_sq();
}
console.log(`point sum: ${s}`);

// Create 500,000 Point3D objects (inherited), call overridden method
let s3d = 0;
for (let j = 0; j < 500_000; j++) {
    const p = new Point3D(j % 100, (j + 1) % 100, (j + 2) % 100);
    s3d += p.distance_sq();
}
console.log(`point3d sum: ${s3d}`);

// isa() checks on 200,000 objects
let isa_count = 0;
for (let k = 0; k < 200_000; k++) {
    const p = new Point3D(1, 2, 3);
    if (p instanceof Point) {
        isa_count += 1;
    }
}
console.log(`isa checks: ${isa_count}`);
