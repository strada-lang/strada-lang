<?php

// Concatenate "hello" 500,000 times
$s = "";
for ($i = 0; $i < 500000; $i++) {
    $s .= "hello";
}
echo "concat len: " . strlen($s) . "\n";

// Split a large string 100,000 times
$csv = "alpha,bravo,charlie,delta,echo,foxtrot,golf,hotel";
$total_parts = 0;
for ($j = 0; $j < 100000; $j++) {
    $parts = explode(",", $csv);
    $total_parts += count($parts);
}
echo "split parts: $total_parts\n";

// Regex replace on a template string 200,000 times
$template = "Hello NAME, welcome to PLACE on DATE";
$result = "";
for ($m = 0; $m < 200000; $m++) {
    $result = $template;
    $result = preg_replace("/NAME/", "World", $result, 1);
    $result = preg_replace("/PLACE/", "Strada", $result, 1);
    $result = preg_replace("/DATE/", "today", $result, 1);
}
echo "regex result: $result\n";
