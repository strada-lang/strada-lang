sub fib { my $n = shift; return $n if $n < 2; return fib($n-1) + fib($n-2); }
my $sum = 0; for my $i (1..100000) { $sum += $i; }
print "sum: $sum\n";
print "fib(25): " . fib(25) . "\n";
