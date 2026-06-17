#!/usr/bin/env perl
# Perl counterpart of bench_closures.strada (identical workload).
use strict; use warnings; use Time::HiRes qw(time);

sub make_adder { my $base = shift; my $bias = $base * 2; return sub { $_[0] + $bias } }
sub make_outer {
    my $seed = shift;
    my $level0 = $seed;
    my $mid = sub { return sub { $_[0] + $level0 } };
    return $mid->();
}

my $t0 = time;
my $made = 0;
for my $i (0..499999) { my $f = make_adder($i); $made++ }
my $t1 = time;
printf "create: %d %.6f\n", $made, $t1 - $t0;

my $add7 = make_adder(7);
my $sum = 0;
$sum += $add7->($_ % 100) for 0..4999999;
my $t2 = time;
printf "invoke: %d %.6f\n", $sum, $t2 - $t1;

my $counter = 0;
my $bump = sub { return ++$counter };
$bump->() for 1..2000000;
my $t3 = time;
printf "capture-rw: %d %.6f\n", $counter, $t3 - $t2;

my %table = map { ("op$_" => make_adder($_)) } 0..15;
my $tsum = 0;
for my $i (0..199999) { $tsum += $table{"op" . ($i % 16)}->($i % 50) }
my $t4 = time;
printf "table: %d %.6f\n", $tsum, $t4 - $t3;

my $deep = make_outer(11);
my $dsum = 0;
$dsum += $deep->($_ % 10) for 0..199999;
my $t5 = time;
printf "transitive: %d %.6f\n", $dsum, $t5 - $t4;
printf "total: %.6f\n", $t5 - $t0;
