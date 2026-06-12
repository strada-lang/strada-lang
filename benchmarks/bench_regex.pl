#!/usr/bin/env perl
# Perl counterpart of bench_regex.strada (identical workload).
use strict; use warnings; use Time::HiRes qw(time);

my @lines;
for my $i (0..199999) {
    my $code = 200 + (($i * 7) % 4) * 100;
    my $bytes = ($i * 37) % 100000;
    push @lines, sprintf('10.0.%d.%d - - [12/Jun/2026:01:0%d:00] "GET /page/%d HTTP/1.1" %d %d',
        int($i/256) % 256, $i % 256, $i % 10, $i % 1000, $code, $bytes);
}

my $t0 = time;
my $hits = 0;
for (@lines) { $hits++ if /" (4|5)\d\d / }
my $t1 = time;
printf "match: %d %.6f\n", $hits, $t1 - $t0;

my $bytes_total = 0;
for (@lines) { $bytes_total += $4 if /^(\S+) .* "(\w+) [^"]*" (\d+) (\d+)$/ }
my $t2 = time;
printf "captures: %d %.6f\n", $bytes_total, $t2 - $t1;

my $code_sum = 0;
for my $i (0..99999) { $code_sum += $+{code} if $lines[$i] =~ /" (?<code>\d+) (?<bytes>\d+)$/ }
my $t3 = time;
printf "named: %d %.6f\n", $code_sum, $t3 - $t2;

my $sub_len = 0;
for my $i (0..99999) { my $l = $lines[$i]; $l =~ s/\d+/N/g; $sub_len += length $l }
my $t4 = time;
printf "subst: %d %.6f\n", $sub_len, $t4 - $t3;

my $sube_len = 0;
for my $i (0..49999) { my $l = $lines[$i]; $l =~ s/(\d+)/length($1)/eg; $sube_len += length $l }
my $t5 = time;
printf "subst-e: %d %.6f\n", $sube_len, $t5 - $t4;

my $tr_count = 0;
for my $i (0..99999) { my $l = $lines[$i]; $tr_count += ($l =~ tr/a-z/A-Z/) }
my $t6 = time;
printf "tr: %d %.6f\n", $tr_count, $t6 - $t5;
printf "total: %.6f\n", $t6 - $t0;
