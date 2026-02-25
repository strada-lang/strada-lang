#!/usr/bin/env perl
use strict;
use warnings;
no warnings 'recursion';

sub add3 {
    return $_[0] + $_[1] + $_[2];
}

sub ackermann {
    my ($m, $n) = @_;
    if ($m == 0) {
        return $n + 1;
    }
    if ($n == 0) {
        return ackermann($m - 1, 1);
    }
    return ackermann($m - 1, ackermann($m, $n - 1));
}

# Call a simple 3-arg function 5,000,000 times
my $sum = 0;
for my $i (0 .. 4_999_999) {
    $sum += add3($i, $i + 1, $i + 2);
}
print "call sum: $sum\n";

# Compute ackermann(3,8)
my $ack = ackermann(3, 8);
print "ackermann(3,8): $ack\n";
