#!/usr/bin/env perl
use strict;
use warnings;

sub fib {
    my ($n) = @_;
    return $n if $n < 2;
    return fib($n - 1) + fib($n - 2);
}

# Sum integers 1 to 50,000,000
my $sum = 0;
for my $i (1 .. 50_000_000) {
    $sum += $i;
}
print "sum: $sum\n";

# Recursive fibonacci(35), run 30 times
my $fib_result = 0;
for my $j (1 .. 30) {
    $fib_result = fib(35);
}
print "fib(35): $fib_result\n";
