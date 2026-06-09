<?php

class Point {
    public int $x;
    public int $y;

    public function __construct(int $x = 0, int $y = 0) {
        $this->x = $x;
        $this->y = $y;
    }

    public function distance_sq(): int {
        return $this->x * $this->x + $this->y * $this->y;
    }
}

class Point3D extends Point {
    public int $z;

    public function __construct(int $x = 0, int $y = 0, int $z = 0) {
        parent::__construct($x, $y);
        $this->z = $z;
    }

    public function distance_sq(): int {
        return $this->x * $this->x + $this->y * $this->y + $this->z * $this->z;
    }
}

// Create 500,000 Point objects, call method on each
$s = 0;
for ($i = 0; $i < 500000; $i++) {
    $p = new Point($i % 100, ($i + 1) % 100);
    $s += $p->distance_sq();
}
echo "point sum: $s\n";

// Create 500,000 Point3D objects (inherited), call overridden method
$s3d = 0;
for ($j = 0; $j < 500000; $j++) {
    $p = new Point3D($j % 100, ($j + 1) % 100, ($j + 2) % 100);
    $s3d += $p->distance_sq();
}
echo "point3d sum: $s3d\n";

// isa() checks on 200,000 objects
$isa_count = 0;
for ($k = 0; $k < 200000; $k++) {
    $p = new Point3D(1, 2, 3);
    if ($p instanceof Point) {
        $isa_count++;
    }
}
echo "isa checks: $isa_count\n";
