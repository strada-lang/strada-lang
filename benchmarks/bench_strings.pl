#!/usr/bin/perl
use strict;
use warnings;

# Concatenate "hello" 500,000 times
my $s = "";
my $i = 0;
while ($i < 500000) {
    $s .= "hello";
    $i++;
}
print "concat len: " . length($s) . "\n";

# Split a large string 100,000 times
my $csv = "alpha,bravo,charlie,delta,echo,foxtrot,golf,hotel";
my $total_parts = 0;
my $j = 0;
while ($j < 100000) {
    my @parts = split(",", $csv);
    $total_parts += scalar(@parts);
    $j++;
}
print "split parts: $total_parts\n";

# Regex replace on a template string 200,000 times
my $template = "Hello NAME, welcome to PLACE on DATE";
my $m = 0;
my $result = "";
while ($m < 200000) {
    $result = $template;
    $result =~ s/NAME/World/;
    $result =~ s/PLACE/Strada/;
    $result =~ s/DATE/today/;
    $m++;
}
print "regex result: $result\n";
