<?php

function add3(int $a, int $b, int $c): int {
    return $a + $b + $c;
}

function ackermann(int $m, int $n): int {
    if ($m == 0) {
        return $n + 1;
    }
    if ($n == 0) {
        return ackermann($m - 1, 1);
    }
    return ackermann($m - 1, ackermann($m, $n - 1));
}

// Call a simple 3-arg function 5,000,000 times
$s = 0;
for ($i = 0; $i < 5000000; $i++) {
    $s += add3($i, $i + 1, $i + 2);
}
echo "call sum: $s\n";

// Compute ackermann(3,8)
$ack = ackermann(3, 8);
echo "ackermann(3,8): $ack\n";
