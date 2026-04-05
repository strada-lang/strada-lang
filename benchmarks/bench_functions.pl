#!/usr/bin/perl
use strict;
use warnings;

sub add3 {
    my ($a, $b, $c) = @_;
    return $a + $b + $c;
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
my $i = 0;
while ($i < 5000000) {
    $sum += add3($i, $i + 1, $i + 2);
    $i++;
}
print "call sum: $sum\n";

# Compute ackermann(3,8)
my $ack = ackermann(3, 8);
print "ackermann(3,8): $ack\n";
