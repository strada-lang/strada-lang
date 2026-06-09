<?php

// Push 2,000,000 integers into an array
$arr = [];
for ($i = 0; $i < 2000000; $i++) {
    $arr[] = $i;
}
echo "array size: " . count($arr) . "\n";

// Sum every 100th element to verify
$s = 0;
for ($j = 0; $j < 2000000; $j += 100) {
    $s += $arr[$j];
}
echo "array checksum: $s\n";

// Insert 500,000 key-value pairs into a hash
$h = [];
for ($k = 0; $k < 500000; $k++) {
    $h["key$k"] = $k;
}
echo "hash size: " . count($h) . "\n";

// Look up all 500,000 values
$lookup_sum = 0;
for ($m = 0; $m < 500000; $m++) {
    $lookup_sum += $h["key$m"];
}
echo "lookup sum: $lookup_sum\n";

// Delete all keys
for ($n = 0; $n < 500000; $n++) {
    unset($h["key$n"]);
}
echo "after delete: " . count($h) . "\n";
