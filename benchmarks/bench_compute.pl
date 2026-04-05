#!/usr/bin/perl
use strict;
use warnings;

sub fib {
    my ($n) = @_;
    if ($n < 2) {
        return $n;
    }
    return fib($n - 1) + fib($n - 2);
}

# Sum integers 1 to 50,000,000
my $sum = 0;
my $i = 1;
while ($i <= 50000000) {
    $sum += $i;
    $i++;
}
print "sum: $sum\n";

# Recursive fibonacci(35), run 30 times
my $fib_result = 0;
my $j = 0;
while ($j < 30) {
    $fib_result = fib(35);
    $j++;
}
print "fib(35): $fib_result\n";
