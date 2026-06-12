#!/usr/bin/env perl
# Perl counterpart of bench_data.strada (identical workload).
use strict; use warnings; use Time::HiRes qw(time);

my $path = "/tmp/perl_bench_data.csv";
my $rows = 500000;
my $t0 = time;

my $seed = 42; my $out = "";
for my $i (0..$rows-1) {
    $seed = ($seed * 1103515245 + 12345) % 2147483648;
    my ($user, $action, $bytes) = ($seed % 5000, int($seed/7) % 6, $seed % 50000);
    $out .= "2026-06-12T01:00:00,user$user,action$action,$bytes,ok\n";
}
open my $wfh, '>', $path or die; print $wfh $out; close $wfh;
my $t1 = time;
printf "generate: %d %.6f\n", $rows, $t1 - $t0;

my (%by_user, %by_action, %bytes_by_user);
my $parsed = 0;
open my $fh, '<', $path or die;
while (my $line = <$fh>) {
    chomp $line;
    my @f = split /,/, $line;
    next if @f < 5;
    $by_user{$f[1]}++; $by_action{$f[2]}++; $bytes_by_user{$f[1]} += $f[3];
    $parsed++;
}
close $fh;
my $t2 = time;
printf "aggregate: %d users=%d %.6f\n", $parsed, scalar(keys %by_user), $t2 - $t1;

my @ranked = sort { $bytes_by_user{$b} <=> $bytes_by_user{$a} } keys %bytes_by_user;
my $report = "";
for my $i (0..19) {
    my $u = $ranked[$i];
    $report .= "$u events=$by_user{$u} bytes=$bytes_by_user{$u}\n";
}
my $t3 = time;
printf "report: %d %.6f\n", length($report), $t3 - $t2;
unlink $path;
printf "total: %.6f\n", $t3 - $t0;
