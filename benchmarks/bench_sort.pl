#!/usr/bin/env perl
# Perl counterpart of bench_sort.strada (identical workload).
use strict; use warnings; use Time::HiRes qw(time);

my $seed = 42;
sub lcg { $seed = ($seed * 1103515245 + 12345) % 2147483648; $seed }

my @ints; push @ints, lcg() % 10000000 for 1..1000000;
my @strs; push @strs, "key_" . (lcg() % 1000000) . "_suffix" for 1..500000;

my $t0 = time;
my @si = sort { $a <=> $b } @ints;
my $t1 = time;
printf "int-sort: %s %.6f\n", $si[0], $t1 - $t0;

my @ss = sort @strs;
my $t2 = time;
printf "str-sort: %s %.6f\n", length($ss[0]), $t2 - $t1;

my @half = @ints[0..499999];
my @sc = sort { $b <=> $a } @half;
my $t3 = time;
printf "cmp-sort: %s %.6f\n", $sc[0], $t3 - $t2;

my %h; $h{"k$_"} = $_ for 0..199999;
my $hsum = 0;
for (1..5) { my @hk = sort keys %h; $hsum += length($hk[0]); }
my $t4 = time;
printf "hash-sort: %s %.6f\n", $hsum, $t4 - $t3;
printf "total: %.6f\n", $t4 - $t0;
