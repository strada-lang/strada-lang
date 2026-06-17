sub add { return $_[0] + $_[1]; }
sub mul { return $_[0] * $_[1]; }
my $sum = 0; for my $i (0..99999) { $sum += add($i, mul($i, 2)); }
print "sum: $sum\n";
