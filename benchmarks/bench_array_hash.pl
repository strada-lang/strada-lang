#!/usr/bin/env perl
use strict;
use warnings;

# Push 2,000,000 integers into an array
my @arr;
for my $i (0 .. 1_999_999) {
    push @arr, $i;
}
print "array size: " . scalar(@arr) . "\n";

# Sum every 100th element to verify
my $sum = 0;
for (my $j = 0; $j < 2_000_000; $j += 100) {
    $sum += $arr[$j];
}
print "array checksum: $sum\n";

# Insert 500,000 key-value pairs into a hash
my %h;
for my $k (0 .. 499_999) {
    $h{"key$k"} = $k;
}
print "hash size: " . scalar(keys %h) . "\n";

# Look up all 500,000 values
my $lookup_sum = 0;
for my $m (0 .. 499_999) {
    $lookup_sum += $h{"key$m"};
}
print "lookup sum: $lookup_sum\n";

# Delete all keys
for my $n (0 .. 499_999) {
    delete $h{"key$n"};
}
print "after delete: " . scalar(keys %h) . "\n";
