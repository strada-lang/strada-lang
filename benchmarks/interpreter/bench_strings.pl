my $s = ""; for my $i (0..9999) { $s .= "hello"; }
print "concat len: " . length($s) . "\n";
my $total = 0; for my $j (0..999) { my @p = split(",", "a,b,c,d,e,f,g,h"); $total += scalar(@p); }
print "split parts: $total\n";
