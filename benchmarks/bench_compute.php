<?php

function fib(int $n): int {
    if ($n < 2) {
        return $n;
    }
    return fib($n - 1) + fib($n - 2);
}

// Sum integers 1 to 50,000,000
$s = 0;
for ($i = 1; $i <= 50000000; $i++) {
    $s += $i;
}
echo "sum: $s\n";

// Recursive fibonacci(35), run 30 times
$fib_result = 0;
for ($j = 0; $j < 30; $j++) {
    $fib_result = fib(35);
}
echo "fib(35): $fib_result\n";
