my @arr; for my $i (0..49999) { push @arr, $i; }
print "array size: " . scalar(@arr) . "\n";
my %h; for my $k (0..9999) { $h{"key$k"} = $k; }
print "hash size: " . scalar(keys %h) . "\n";
