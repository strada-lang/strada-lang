#!/usr/bin/env perl
use strict;
use warnings;

# Concatenate "hello" 500,000 times
my $s = "";
for my $i (1 .. 500_000) {
    $s .= "hello";
}
print "concat len: " . length($s) . "\n";

# Split a large string 100,000 times
my $csv = "alpha,bravo,charlie,delta,echo,foxtrot,golf,hotel";
my $total_parts = 0;
for my $j (1 .. 100_000) {
    my @parts = split(/,/, $csv);
    $total_parts += scalar(@parts);
}
print "split parts: $total_parts\n";

# Regex replace on a template string 200,000 times
my $template = "Hello NAME, welcome to PLACE on DATE";
my $result = "";
for my $m (1 .. 200_000) {
    $result = $template;
    $result =~ s/NAME/World/;
    $result =~ s/PLACE/Strada/;
    $result =~ s/DATE/today/;
}
print "regex result: $result\n";
