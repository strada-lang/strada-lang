#!/usr/bin/perl
use strict;
use warnings;
use Time::HiRes qw(time);

# Perl counterpart of bench_oop_so.strada. Perl has no shared-library /
# same-binary distinction for method dispatch — every method call resolves
# through the symbol table the same way — so the "local" and "so" sections
# run the IDENTICAL class. The workload (loop counts, methods, arithmetic)
# matches bench_oop_so.strada section-for-section, so Perl's whole-program
# time is directly comparable to the Strada total, and each Perl section is
# comparable to the corresponding Strada local/.so section (they are equal
# in Perl). Sums printed below match the Strada benchmark exactly.

package Counter;

sub new {
    my ($class, $start) = @_;
    return bless { n => $start, step => 1 }, $class;
}

sub bump  { my $self = shift; $self->{n} += $self->{step}; return $self->{n}; }
sub value { return $_[0]->{n}; }
sub add   { my ($self, $k) = @_; $self->{n} += $k; return $self->{n}; }

package main;

my $t0 = time();

# 1. "local" method calls — 1,000,000 bump()
my $lc = Counter->new(0);
my $lsum = 0;
for (my $i = 0; $i < 1000000; $i++) { $lsum = $lc->bump(); }
my $t1 = time();
printf "local-call: %d %.6f\n", $lsum, $t1 - $t0;

# 2. "so" method calls — identical workload (no .so concept in Perl)
my $sc = Counter->new(0);
my $ssum = 0;
for (my $i = 0; $i < 1000000; $i++) { $ssum = $sc->bump(); }
my $t2 = time();
printf "so-call: %d %.6f\n", $ssum, $t2 - $t1;

# 3. "local" construction — 200,000 new + value()
my $lmade = 0;
for (my $i = 0; $i < 200000; $i++) { my $o = Counter->new($i); $lmade += $o->value(); }
my $t3 = time();
printf "local-new: %d %.6f\n", $lmade, $t3 - $t2;

# 4. "so" construction — identical workload
my $smade = 0;
for (my $i = 0; $i < 200000; $i++) { my $o = Counter->new($i); $smade += $o->value(); }
my $t4 = time();
printf "so-new: %d %.6f\n", $smade, $t4 - $t3;

# 5. "local" mixed dispatch — 333,333 iters of bump/add(2)/value
my $lm = Counter->new(0);
my $lmix = 0;
for (my $i = 0; $i < 333333; $i++) { $lm->bump(); $lm->add(2); $lmix = $lm->value(); }
my $t5 = time();
printf "local-mixed: %d %.6f\n", $lmix, $t5 - $t4;

# 6. "so" mixed dispatch — identical workload
my $sm = Counter->new(0);
my $smix = 0;
for (my $i = 0; $i < 333333; $i++) { $sm->bump(); $sm->add(2); $smix = $sm->value(); }
my $t6 = time();
printf "so-mixed: %d %.6f\n", $smix, $t6 - $t5;

printf "total: %.6f\n", $t6 - $t0;
