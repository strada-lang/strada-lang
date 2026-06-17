#!/usr/bin/env perl
# Perl counterpart of bench_binary_trees.strada (identical workload).
use strict; use warnings; use Time::HiRes qw(time);

sub build { my $d = shift; return $d == 0 ? { l => undef, r => undef } : { l => build($d-1), r => build($d-1) } }
sub check { my $n = shift; return defined $n->{l} ? 1 + check($n->{l}) + check($n->{r}) : 1 }

my $max_depth = 16;
my $t0 = time;
my $stretch = build($max_depth + 1);
my $sc = check($stretch);
undef $stretch;
my $t1 = time;
printf "stretch: %d %.6f\n", $sc, $t1 - $t0;

my $long_lived = build($max_depth);
my $t2 = time;
printf "long-lived: built %.6f\n", $t2 - $t1;

for (my $depth = 4; $depth <= 14; $depth += 2) {
    my $iters = 1 << ($max_depth - $depth + 2);
    my $sum = 0;
    $sum += check(build($depth)) for 1..$iters;
    print "depth $depth: $iters trees, check $sum\n";
}
my $t3 = time;
printf "iterate: %d %.6f\n", check($long_lived), $t3 - $t2;
printf "total: %.6f\n", $t3 - $t0;
