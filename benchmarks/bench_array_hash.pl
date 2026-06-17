#!/usr/bin/perl
use strict;
use warnings;

# Push 2,000,000 integers into an array
my @arr;
my $i = 0;
while ($i < 2000000) {
    push(@arr, $i);
    $i++;
}
print "array size: " . scalar(@arr) . "\n";

# Sum every 100th element to verify
my $sum = 0;
my $j = 0;
while ($j < 2000000) {
    $sum += $arr[$j];
    $j += 100;
}
print "array checksum: $sum\n";

# Insert 500,000 key-value pairs into a hash
my %h;
my $k = 0;
while ($k < 500000) {
    $h{"key" . $k} = $k;
    $k++;
}
print "hash size: " . scalar(keys(%h)) . "\n";

# Look up all 500,000 values
my $lookup_sum = 0;
my $m = 0;
while ($m < 500000) {
    $lookup_sum += $h{"key" . $m};
    $m++;
}
print "lookup sum: $lookup_sum\n";

# Delete all keys
my $n = 0;
while ($n < 500000) {
    delete($h{"key" . $n});
    $n++;
}
print "after delete: " . scalar(keys(%h)) . "\n";
