#!/usr/bin/env perl
# Perl counterpart of bench_exceptions.strada (eval/die; typed catch is
# emulated with ref() checks, the idiomatic Perl equivalent).
use strict; use warnings; use Time::HiRes qw(time);

my $t0 = time;
my $ok = 0;
for (1..2000000) { eval { $ok++ }; $ok-- if $@ }
my $t1 = time;
printf "try-nothrow: %d %.6f\n", $ok, $t1 - $t0;

my $caught = 0;
for my $i (1..200000) { eval { die "boom $i\n" }; $caught++ if $@ }
my $t2 = time;
printf "throw-catch: %d %.6f\n", $caught, $t2 - $t1;

my $typed = 0;
for (1..200000) {
    eval { die bless {}, 'WantedErr' };
    if (ref $@ eq 'MiscErr') { $typed-- }
    elsif (ref $@ eq 'WantedErr') { $typed++ }
    else { $typed-- }
}
my $t3 = time;
printf "typed: %d %.6f\n", $typed, $t3 - $t2;

sub deep { my $n = shift; die "bottom\n" if $n <= 0; return deep($n - 1) + 1 }
my $deep_caught = 0;
for (1..20000) { eval { deep(50) }; $deep_caught++ if $@ }
my $t4 = time;
printf "deep-unwind: %d %.6f\n", $deep_caught, $t4 - $t3;

my $fin = 0;
for (1..1000000) { eval { $fin++ }; $fin++ }   # eval + unconditional cleanup
my $t5 = time;
printf "finally: %d %.6f\n", $fin, $t5 - $t4;
printf "total: %.6f\n", $t5 - $t0;
