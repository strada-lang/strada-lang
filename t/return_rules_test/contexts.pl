use strict; no warnings;
our @G = (3, 1, 2);
sub lst { return @G; } sub ref_ { return [3, 1, 2]; } sub pairs { return ("a", 1, "b", 2); }
sub hsh { my %h = ("a" => 1, "b" => 2); return %h; } sub uh { my %h = ("a" => 1); return %h; }
sub none { return; } sub und { return undef; } sub noret { my $x = 1; }
sub cmade { return [7, 8]; }
sub take { return scalar(@_); } sub one { my $v = shift; return defined($v) ? (length(ref($v)) ? ref($v) : "" . $v) : "u"; }
sub cnt { return scalar(@_); } sub pass { return lst(); } sub typed { return (7, 8, 9); } sub tref { my $r = [1, 2]; return @$r; }
package O; sub new { bless {}, "O" } sub cnt { my $s = shift; scalar(@_) } sub w { wantarray() ? "L" : "S" }
package main;
my $o = O->new;
my (@t1, @t2, @t3, @t4);
print "sort " . join(",", sort(lst())) . " " . scalar(@t1 = sort(ref_())) . "\n";
print "map " . join(",", map { $_ * 2 } lst()) . " grep " . scalar(@t2 = grep { $_ > 1 } lst()) . "\n";
print "rev " . join(",", reverse(lst())) . "\n";
my %h = pairs(); print "hash " . join(",", sort(keys(%h))) . "\n";
my %h2 = hsh(); print "hash2 " . join(",", sort(keys(%h2))) . "\n";
my $hr = { "z", 0, pairs() }; print "anonh " . join(",", sort(keys(%$hr))) . "\n";
my @hl = (0, hsh(), 9); print "hashitem " . scalar(@hl) . "\n";
my @ul = (0, uh(), 9); print "uhashitem " . scalar(@ul) . "\n";
print "concat n=" . lst() . " " . pairs() . "\n";
print "arith " . (lst() + 10) . " " . (typed() + 0) . "\n";
print "typed " . scalar(@t3 = typed()) . " " . cnt(typed()) . " " . cnt(tref()) . "\n";
my $ts = typed(); my $trs = tref(); print "typedscalar " . $ts . " " . $trs . "\n";
print "scalar() " . scalar(lst()) . "\n";
print "defined " . (defined(none()) ? 1 : 0) . (defined(und()) ? 1 : 0) . (defined(lst()) ? 1 : 0) . "\n";
my @n1 = none(); my @n2 = und(); my @n3 = (0, none(), 9); my @n4 = (0, und(), 9); my @n5 = noret();
print "none " . scalar(@n1) . scalar(@n2) . scalar(@n3) . scalar(@n4) . scalar(@n5) . "\n";
print "cnt " . cnt(none()) . cnt(und()) . cnt(lst()) . cnt(ref_()) . " m " . $o->cnt(none()) . $o->cnt(und()) . $o->cnt(lst()) . $o->cnt(ref_()) . "\n";
print "take " . take(lst()) . " one " . one(ref_()) . " " . one(none()) . " " . one(und()) . "\n";
print "pass " . cnt(pass()) . " " . scalar(@t4 = pass()) . "\n";
my $ps = pass(); print "passS " . $ps . "\n";
my $cm = cmade(); print "cmade " . ref($cm) . " " . scalar(@$cm) . " " . cnt(cmade()) . "\n";
my @w = ($o->w()); my $ws = $o->w(); my @w2 = $o->w(); print "want " . $w[0] . $ws . $w2[0] . " " . ($o->w() . "") . "\n";
my $cl = sub { return scalar(@_); };
print "clos " . $cl->(lst()) . $cl->(ref_()) . $cl->(0, lst(), 9) . "\n";
my $t = lst() ? "T" : "F"; my $u = none() ? "T" : "F"; print "tern " . $t . $u . "\n";
my ($a, $b) = (lst())[0, 1]; print "slice " . $a . $b . "\n";
foreach my $e (lst(), ref_()) { print(ref($e) eq "" ? $e : "R"); } print "\n";
print "idx " . ref_()->[1] . " " . scalar(@{ref_()}) . "\n";
