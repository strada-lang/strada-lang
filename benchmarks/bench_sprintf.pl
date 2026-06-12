#!/usr/bin/env perl
# Perl counterpart of bench_sprintf.strada (identical workload).
use strict; use warnings; use Time::HiRes qw(time);

my $t0 = time;
my $mlen = 0;
for my $i (0..499999) {
    $mlen += length sprintf("[%s] user=%s req=%d bytes=%d t=%.2fms", "INFO", "u" . ($i % 1000), $i, ($i * 37) % 100000, ($i % 500) / 7.0);
}
my $t1 = time;
printf "mixed: %d %.6f\n", $mlen, $t1 - $t0;

my $nlen = 0;
$nlen += length sprintf("%d %x %o", $_, $_, $_) for 0..999999;
my $t2 = time;
printf "numeric: %d %.6f\n", $nlen, $t2 - $t1;

my $flen = 0;
for my $i (0..499999) { my $v = $i / 3.0; $flen += length sprintf("%.6f %e %g", $v, $v, $v) }
my $t3 = time;
printf "float: %d %.6f\n", $flen, $t3 - $t2;

my $wlen = 0;
for my $i (0..499999) {
    $wlen += length sprintf("%-20s|%10d|%08d|%6.1f%%", "row_" . ($i % 100), $i, $i % 1000, ($i % 1000) / 10.0);
}
my $t4 = time;
printf "width: %d %.6f\n", $wlen, $t4 - $t3;
printf "total: %.6f\n", $t4 - $t0;
