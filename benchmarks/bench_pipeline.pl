#!/usr/bin/env perl
# Perl counterpart of bench_pipeline.strada (identical workload).
use strict; use warnings; use Time::HiRes qw(time);

my $t0 = time;
my @squares = map { $_ * $_ } (1..2000000);
my $t1 = time;
printf "map-range: %s %.6f\n", $squares[1999999], $t1 - $t0;

my @mults = grep { $_ % 7 == 0 } (1..2000000);
my $t2 = time;
printf "grep-range: %d %.6f\n", scalar(@mults), $t2 - $t1;

my @base; my $seed = 42;
for (1..300000) { $seed = ($seed * 1103515245 + 12345) % 2147483648; push @base, $seed % 1000000 }
my @mapped = map { $_ * 3 + 1 } @base;
my @kept = grep { $_ % 2 == 1 } @mapped;
my @ordered = sort { $b <=> $a } @kept;
my $top = 0; $top += $ordered[$_] for 0..99;
my $t3 = time;
printf "chain: %d %.6f\n", $top, $t3 - $t2;

my @words; push @words, "w" . ($_ % 1000) for 0..499999;
my $jlen = 0;
for (1..3) { my $j = join(",", @words); $jlen += length $j }
my $t4 = time;
printf "join: %d %.6f\n", $jlen, $t4 - $t3;

my $csv = join(",", @words);
my $slen = 0;
for (1..3) { my @parts = split /,/, $csv; $slen += scalar @parts }
my $t5 = time;
printf "split-join: %d %.6f\n", $slen, $t5 - $t4;
printf "total: %.6f\n", $t5 - $t0;
