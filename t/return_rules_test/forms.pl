use strict; use warnings; no warnings qw(once);
our @G = (1, 2, 3); our $R = [4, 5]; our @A2 = (1, 2); our @B2 = (3); our %H = (); our $T = 1; our $F = 0;
sub desc { my $v = shift; return "u" if !defined($v); return ref($v) if length(ref($v)); return "" . $v; }
sub cnt { return scalar(@_); }
package Box;
sub new { return bless({ items => [1, 2, 3] }, "Box"); }
sub cnt { my $self = shift; return scalar(@_); }
sub m_local { my $self = shift; my @p = (1, 2, 3); return @p; }
sub m_aref { return [1, 2, 3]; }
sub m_items { my $self = shift; return $self->{"items"}; }
sub m_all { my $self = shift; return @{$self->{"items"}}; }
sub m_arr { return (1, 2, 3); }
sub a_local { my $self = shift; my @p = (1, 2, 3); return @p; }
sub a_aref { my $self = shift; return [1, 2, 3]; }
sub w_ctx { my $self = shift; return wantarray() ? "list" : "scalar"; }
sub hooked { return (1, 2, 3); }
our $AUTOLOAD;
sub AUTOLOAD { my $self = shift; my $m = $AUTOLOAD; $m =~ s/.*:://; return if $m eq "DESTROY"; if ($m eq "au_list") { return (1, 2, 3); } return [1, 2, 3]; }
package main;
sub f_local { my @p = (1, 2, 3); return @p; }
sub f_our { return @G; }
sub f_deref { return @{$R}; }
sub f_comma { return (7, 8, 9); }
sub f_two { return (@A2, @B2); }
sub f_empty { return (); }
sub f_bare { return; }
sub f_aref { return [1, 2, 3]; }
sub f_refto { return \@G; }
sub f_sref { my $r = [1, 2, 3]; return $r; }
sub f_elem { return $H{"k"}; }
sub f_elemv { return $H{"v"}; }
sub f_href { return {"a" => 1}; }
sub f_map { return map { $_ * 2 } @G; }
sub f_ternL { return $T ? @G : [9]; }
sub f_ternS { return $F ? @G : [9]; }
sub f_call { return f_local(); }
sub f_callS { return f_aref(); }
sub f_scal { return 5; }
sub f_str { return "s"; }
my $B = Box->new;
$H{"k"} = [1, 2];
push(@{$H{"v"}}, 1); push(@{$H{"v"}}, 2);
for my $round (1 .. 2) {
    my $CL = sub { my @p = (1, 2, 3); return @p; };
    my $CS = sub { return [1, 2, 3]; };
    my $CR = \&f_local;
    my $MN = "m_local";
    { my @x = (0, f_local(), 9); print("f_local item " . scalar(@x) . "\n"); }
    { my @x = f_local(); print("f_local arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_local(); print("f_local destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_local()) { $c++; } print("f_local foreach " . $c . "\n"); }
    print("f_local variadic " . cnt(f_local()) . "\n");
    print("f_local marg " . $B->cnt(f_local()) . "\n");
    { my $ar = [f_local()]; print("f_local anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_local()); print("f_local push " . scalar(@x) . "\n"); }
    { my $n = f_local(); print("f_local scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_local(); print("f_local store " . desc($hh{"n"}) . "\n"); }
    if (f_local()) { print("f_local if T\n"); } else { print("f_local if F\n"); }
    { my @x = (0, f_our(), 9); print("f_our item " . scalar(@x) . "\n"); }
    { my @x = f_our(); print("f_our arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_our(); print("f_our destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_our()) { $c++; } print("f_our foreach " . $c . "\n"); }
    print("f_our variadic " . cnt(f_our()) . "\n");
    print("f_our marg " . $B->cnt(f_our()) . "\n");
    { my $ar = [f_our()]; print("f_our anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_our()); print("f_our push " . scalar(@x) . "\n"); }
    { my $n = f_our(); print("f_our scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_our(); print("f_our store " . desc($hh{"n"}) . "\n"); }
    if (f_our()) { print("f_our if T\n"); } else { print("f_our if F\n"); }
    { my @x = (0, f_deref(), 9); print("f_deref item " . scalar(@x) . "\n"); }
    { my @x = f_deref(); print("f_deref arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_deref(); print("f_deref destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_deref()) { $c++; } print("f_deref foreach " . $c . "\n"); }
    print("f_deref variadic " . cnt(f_deref()) . "\n");
    print("f_deref marg " . $B->cnt(f_deref()) . "\n");
    { my $ar = [f_deref()]; print("f_deref anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_deref()); print("f_deref push " . scalar(@x) . "\n"); }
    { my $n = f_deref(); print("f_deref scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_deref(); print("f_deref store " . desc($hh{"n"}) . "\n"); }
    if (f_deref()) { print("f_deref if T\n"); } else { print("f_deref if F\n"); }
    { my @x = (0, f_comma(), 9); print("f_comma item " . scalar(@x) . "\n"); }
    { my @x = f_comma(); print("f_comma arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_comma(); print("f_comma destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_comma()) { $c++; } print("f_comma foreach " . $c . "\n"); }
    print("f_comma variadic " . cnt(f_comma()) . "\n");
    print("f_comma marg " . $B->cnt(f_comma()) . "\n");
    { my $ar = [f_comma()]; print("f_comma anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_comma()); print("f_comma push " . scalar(@x) . "\n"); }
    { my $n = f_comma(); print("f_comma scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_comma(); print("f_comma store " . desc($hh{"n"}) . "\n"); }
    if (f_comma()) { print("f_comma if T\n"); } else { print("f_comma if F\n"); }
    { my @x = (0, f_two(), 9); print("f_two item " . scalar(@x) . "\n"); }
    { my @x = f_two(); print("f_two arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_two(); print("f_two destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_two()) { $c++; } print("f_two foreach " . $c . "\n"); }
    print("f_two variadic " . cnt(f_two()) . "\n");
    print("f_two marg " . $B->cnt(f_two()) . "\n");
    { my $ar = [f_two()]; print("f_two anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_two()); print("f_two push " . scalar(@x) . "\n"); }
    { my $n = f_two(); print("f_two scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_two(); print("f_two store " . desc($hh{"n"}) . "\n"); }
    if (f_two()) { print("f_two if T\n"); } else { print("f_two if F\n"); }
    { my @x = (0, f_empty(), 9); print("f_empty item " . scalar(@x) . "\n"); }
    { my @x = f_empty(); print("f_empty arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_empty(); print("f_empty destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_empty()) { $c++; } print("f_empty foreach " . $c . "\n"); }
    print("f_empty variadic " . cnt(f_empty()) . "\n");
    print("f_empty marg " . $B->cnt(f_empty()) . "\n");
    { my $ar = [f_empty()]; print("f_empty anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_empty()); print("f_empty push " . scalar(@x) . "\n"); }
    { my $n = f_empty(); print("f_empty scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_empty(); print("f_empty store " . desc($hh{"n"}) . "\n"); }
    if (f_empty()) { print("f_empty if T\n"); } else { print("f_empty if F\n"); }
    { my @x = (0, f_bare(), 9); print("f_bare item " . scalar(@x) . "\n"); }
    { my @x = f_bare(); print("f_bare arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_bare(); print("f_bare destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_bare()) { $c++; } print("f_bare foreach " . $c . "\n"); }
    print("f_bare variadic " . cnt(f_bare()) . "\n");
    print("f_bare marg " . $B->cnt(f_bare()) . "\n");
    { my $ar = [f_bare()]; print("f_bare anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_bare()); print("f_bare push " . scalar(@x) . "\n"); }
    { my $n = f_bare(); print("f_bare scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_bare(); print("f_bare store " . desc($hh{"n"}) . "\n"); }
    if (f_bare()) { print("f_bare if T\n"); } else { print("f_bare if F\n"); }
    { my @x = (0, f_aref(), 9); print("f_aref item " . scalar(@x) . "\n"); }
    { my @x = f_aref(); print("f_aref arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_aref(); print("f_aref destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_aref()) { $c++; } print("f_aref foreach " . $c . "\n"); }
    print("f_aref variadic " . cnt(f_aref()) . "\n");
    print("f_aref marg " . $B->cnt(f_aref()) . "\n");
    { my $ar = [f_aref()]; print("f_aref anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_aref()); print("f_aref push " . scalar(@x) . "\n"); }
    { my $n = f_aref(); print("f_aref scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_aref(); print("f_aref store " . desc($hh{"n"}) . "\n"); }
    if (f_aref()) { print("f_aref if T\n"); } else { print("f_aref if F\n"); }
    { my @x = (0, f_refto(), 9); print("f_refto item " . scalar(@x) . "\n"); }
    { my @x = f_refto(); print("f_refto arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_refto(); print("f_refto destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_refto()) { $c++; } print("f_refto foreach " . $c . "\n"); }
    print("f_refto variadic " . cnt(f_refto()) . "\n");
    print("f_refto marg " . $B->cnt(f_refto()) . "\n");
    { my $ar = [f_refto()]; print("f_refto anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_refto()); print("f_refto push " . scalar(@x) . "\n"); }
    { my $n = f_refto(); print("f_refto scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_refto(); print("f_refto store " . desc($hh{"n"}) . "\n"); }
    if (f_refto()) { print("f_refto if T\n"); } else { print("f_refto if F\n"); }
    { my @x = (0, f_sref(), 9); print("f_sref item " . scalar(@x) . "\n"); }
    { my @x = f_sref(); print("f_sref arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_sref(); print("f_sref destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_sref()) { $c++; } print("f_sref foreach " . $c . "\n"); }
    print("f_sref variadic " . cnt(f_sref()) . "\n");
    print("f_sref marg " . $B->cnt(f_sref()) . "\n");
    { my $ar = [f_sref()]; print("f_sref anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_sref()); print("f_sref push " . scalar(@x) . "\n"); }
    { my $n = f_sref(); print("f_sref scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_sref(); print("f_sref store " . desc($hh{"n"}) . "\n"); }
    if (f_sref()) { print("f_sref if T\n"); } else { print("f_sref if F\n"); }
    { my @x = (0, f_elem(), 9); print("f_elem item " . scalar(@x) . "\n"); }
    { my @x = f_elem(); print("f_elem arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_elem(); print("f_elem destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_elem()) { $c++; } print("f_elem foreach " . $c . "\n"); }
    print("f_elem variadic " . cnt(f_elem()) . "\n");
    print("f_elem marg " . $B->cnt(f_elem()) . "\n");
    { my $ar = [f_elem()]; print("f_elem anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_elem()); print("f_elem push " . scalar(@x) . "\n"); }
    { my $n = f_elem(); print("f_elem scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_elem(); print("f_elem store " . desc($hh{"n"}) . "\n"); }
    if (f_elem()) { print("f_elem if T\n"); } else { print("f_elem if F\n"); }
    { my @x = (0, f_elemv(), 9); print("f_elemv item " . scalar(@x) . "\n"); }
    { my @x = f_elemv(); print("f_elemv arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_elemv(); print("f_elemv destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_elemv()) { $c++; } print("f_elemv foreach " . $c . "\n"); }
    print("f_elemv variadic " . cnt(f_elemv()) . "\n");
    print("f_elemv marg " . $B->cnt(f_elemv()) . "\n");
    { my $ar = [f_elemv()]; print("f_elemv anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_elemv()); print("f_elemv push " . scalar(@x) . "\n"); }
    { my $n = f_elemv(); print("f_elemv scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_elemv(); print("f_elemv store " . desc($hh{"n"}) . "\n"); }
    if (f_elemv()) { print("f_elemv if T\n"); } else { print("f_elemv if F\n"); }
    { my @x = (0, f_href(), 9); print("f_href item " . scalar(@x) . "\n"); }
    { my @x = f_href(); print("f_href arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_href(); print("f_href destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_href()) { $c++; } print("f_href foreach " . $c . "\n"); }
    print("f_href variadic " . cnt(f_href()) . "\n");
    print("f_href marg " . $B->cnt(f_href()) . "\n");
    { my $ar = [f_href()]; print("f_href anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_href()); print("f_href push " . scalar(@x) . "\n"); }
    { my $n = f_href(); print("f_href scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_href(); print("f_href store " . desc($hh{"n"}) . "\n"); }
    if (f_href()) { print("f_href if T\n"); } else { print("f_href if F\n"); }
    { my @x = (0, f_map(), 9); print("f_map item " . scalar(@x) . "\n"); }
    { my @x = f_map(); print("f_map arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_map(); print("f_map destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_map()) { $c++; } print("f_map foreach " . $c . "\n"); }
    print("f_map variadic " . cnt(f_map()) . "\n");
    print("f_map marg " . $B->cnt(f_map()) . "\n");
    { my $ar = [f_map()]; print("f_map anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_map()); print("f_map push " . scalar(@x) . "\n"); }
    { my $n = f_map(); print("f_map scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_map(); print("f_map store " . desc($hh{"n"}) . "\n"); }
    if (f_map()) { print("f_map if T\n"); } else { print("f_map if F\n"); }
    { my @x = (0, f_ternL(), 9); print("f_ternL item " . scalar(@x) . "\n"); }
    { my @x = f_ternL(); print("f_ternL arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_ternL(); print("f_ternL destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_ternL()) { $c++; } print("f_ternL foreach " . $c . "\n"); }
    print("f_ternL variadic " . cnt(f_ternL()) . "\n");
    print("f_ternL marg " . $B->cnt(f_ternL()) . "\n");
    { my $ar = [f_ternL()]; print("f_ternL anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_ternL()); print("f_ternL push " . scalar(@x) . "\n"); }
    { my $n = f_ternL(); print("f_ternL scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_ternL(); print("f_ternL store " . desc($hh{"n"}) . "\n"); }
    if (f_ternL()) { print("f_ternL if T\n"); } else { print("f_ternL if F\n"); }
    { my @x = (0, f_ternS(), 9); print("f_ternS item " . scalar(@x) . "\n"); }
    { my @x = f_ternS(); print("f_ternS arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_ternS(); print("f_ternS destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_ternS()) { $c++; } print("f_ternS foreach " . $c . "\n"); }
    print("f_ternS variadic " . cnt(f_ternS()) . "\n");
    print("f_ternS marg " . $B->cnt(f_ternS()) . "\n");
    { my $ar = [f_ternS()]; print("f_ternS anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_ternS()); print("f_ternS push " . scalar(@x) . "\n"); }
    { my $n = f_ternS(); print("f_ternS scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_ternS(); print("f_ternS store " . desc($hh{"n"}) . "\n"); }
    if (f_ternS()) { print("f_ternS if T\n"); } else { print("f_ternS if F\n"); }
    { my @x = (0, f_call(), 9); print("f_call item " . scalar(@x) . "\n"); }
    { my @x = f_call(); print("f_call arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_call(); print("f_call destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_call()) { $c++; } print("f_call foreach " . $c . "\n"); }
    print("f_call variadic " . cnt(f_call()) . "\n");
    print("f_call marg " . $B->cnt(f_call()) . "\n");
    { my $ar = [f_call()]; print("f_call anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_call()); print("f_call push " . scalar(@x) . "\n"); }
    { my $n = f_call(); print("f_call scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_call(); print("f_call store " . desc($hh{"n"}) . "\n"); }
    if (f_call()) { print("f_call if T\n"); } else { print("f_call if F\n"); }
    { my @x = (0, f_callS(), 9); print("f_callS item " . scalar(@x) . "\n"); }
    { my @x = f_callS(); print("f_callS arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_callS(); print("f_callS destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_callS()) { $c++; } print("f_callS foreach " . $c . "\n"); }
    print("f_callS variadic " . cnt(f_callS()) . "\n");
    print("f_callS marg " . $B->cnt(f_callS()) . "\n");
    { my $ar = [f_callS()]; print("f_callS anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_callS()); print("f_callS push " . scalar(@x) . "\n"); }
    { my $n = f_callS(); print("f_callS scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_callS(); print("f_callS store " . desc($hh{"n"}) . "\n"); }
    if (f_callS()) { print("f_callS if T\n"); } else { print("f_callS if F\n"); }
    { my @x = (0, f_scal(), 9); print("f_scal item " . scalar(@x) . "\n"); }
    { my @x = f_scal(); print("f_scal arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_scal(); print("f_scal destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_scal()) { $c++; } print("f_scal foreach " . $c . "\n"); }
    print("f_scal variadic " . cnt(f_scal()) . "\n");
    print("f_scal marg " . $B->cnt(f_scal()) . "\n");
    { my $ar = [f_scal()]; print("f_scal anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_scal()); print("f_scal push " . scalar(@x) . "\n"); }
    { my $n = f_scal(); print("f_scal scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_scal(); print("f_scal store " . desc($hh{"n"}) . "\n"); }
    if (f_scal()) { print("f_scal if T\n"); } else { print("f_scal if F\n"); }
    { my @x = (0, f_str(), 9); print("f_str item " . scalar(@x) . "\n"); }
    { my @x = f_str(); print("f_str arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = f_str(); print("f_str destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e (f_str()) { $c++; } print("f_str foreach " . $c . "\n"); }
    print("f_str variadic " . cnt(f_str()) . "\n");
    print("f_str marg " . $B->cnt(f_str()) . "\n");
    { my $ar = [f_str()]; print("f_str anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, f_str()); print("f_str push " . scalar(@x) . "\n"); }
    { my $n = f_str(); print("f_str scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = f_str(); print("f_str store " . desc($hh{"n"}) . "\n"); }
    if (f_str()) { print("f_str if T\n"); } else { print("f_str if F\n"); }
    { my @x = (0, $B->m_local(), 9); print("m_local item " . scalar(@x) . "\n"); }
    { my @x = $B->m_local(); print("m_local arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->m_local(); print("m_local destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->m_local()) { $c++; } print("m_local foreach " . $c . "\n"); }
    print("m_local variadic " . cnt($B->m_local()) . "\n");
    print("m_local marg " . $B->cnt($B->m_local()) . "\n");
    { my $ar = [$B->m_local()]; print("m_local anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->m_local()); print("m_local push " . scalar(@x) . "\n"); }
    { my $n = $B->m_local(); print("m_local scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->m_local(); print("m_local store " . desc($hh{"n"}) . "\n"); }
    if ($B->m_local()) { print("m_local if T\n"); } else { print("m_local if F\n"); }
    { my @x = (0, $B->m_aref(), 9); print("m_aref item " . scalar(@x) . "\n"); }
    { my @x = $B->m_aref(); print("m_aref arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->m_aref(); print("m_aref destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->m_aref()) { $c++; } print("m_aref foreach " . $c . "\n"); }
    print("m_aref variadic " . cnt($B->m_aref()) . "\n");
    print("m_aref marg " . $B->cnt($B->m_aref()) . "\n");
    { my $ar = [$B->m_aref()]; print("m_aref anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->m_aref()); print("m_aref push " . scalar(@x) . "\n"); }
    { my $n = $B->m_aref(); print("m_aref scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->m_aref(); print("m_aref store " . desc($hh{"n"}) . "\n"); }
    if ($B->m_aref()) { print("m_aref if T\n"); } else { print("m_aref if F\n"); }
    { my @x = (0, $B->m_items(), 9); print("m_items item " . scalar(@x) . "\n"); }
    { my @x = $B->m_items(); print("m_items arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->m_items(); print("m_items destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->m_items()) { $c++; } print("m_items foreach " . $c . "\n"); }
    print("m_items variadic " . cnt($B->m_items()) . "\n");
    print("m_items marg " . $B->cnt($B->m_items()) . "\n");
    { my $ar = [$B->m_items()]; print("m_items anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->m_items()); print("m_items push " . scalar(@x) . "\n"); }
    { my $n = $B->m_items(); print("m_items scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->m_items(); print("m_items store " . desc($hh{"n"}) . "\n"); }
    if ($B->m_items()) { print("m_items if T\n"); } else { print("m_items if F\n"); }
    { my @x = (0, $B->m_all(), 9); print("m_all item " . scalar(@x) . "\n"); }
    { my @x = $B->m_all(); print("m_all arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->m_all(); print("m_all destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->m_all()) { $c++; } print("m_all foreach " . $c . "\n"); }
    print("m_all variadic " . cnt($B->m_all()) . "\n");
    print("m_all marg " . $B->cnt($B->m_all()) . "\n");
    { my $ar = [$B->m_all()]; print("m_all anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->m_all()); print("m_all push " . scalar(@x) . "\n"); }
    { my $n = $B->m_all(); print("m_all scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->m_all(); print("m_all store " . desc($hh{"n"}) . "\n"); }
    if ($B->m_all()) { print("m_all if T\n"); } else { print("m_all if F\n"); }
    { my @x = (0, $B->m_arr(), 9); print("m_arr item " . scalar(@x) . "\n"); }
    { my @x = $B->m_arr(); print("m_arr arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->m_arr(); print("m_arr destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->m_arr()) { $c++; } print("m_arr foreach " . $c . "\n"); }
    print("m_arr variadic " . cnt($B->m_arr()) . "\n");
    print("m_arr marg " . $B->cnt($B->m_arr()) . "\n");
    { my $ar = [$B->m_arr()]; print("m_arr anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->m_arr()); print("m_arr push " . scalar(@x) . "\n"); }
    { my $n = $B->m_arr(); print("m_arr scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->m_arr(); print("m_arr store " . desc($hh{"n"}) . "\n"); }
    if ($B->m_arr()) { print("m_arr if T\n"); } else { print("m_arr if F\n"); }
    { my @x = (0, $B->a_local(), 9); print("a_local item " . scalar(@x) . "\n"); }
    { my @x = $B->a_local(); print("a_local arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->a_local(); print("a_local destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->a_local()) { $c++; } print("a_local foreach " . $c . "\n"); }
    print("a_local variadic " . cnt($B->a_local()) . "\n");
    print("a_local marg " . $B->cnt($B->a_local()) . "\n");
    { my $ar = [$B->a_local()]; print("a_local anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->a_local()); print("a_local push " . scalar(@x) . "\n"); }
    { my $n = $B->a_local(); print("a_local scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->a_local(); print("a_local store " . desc($hh{"n"}) . "\n"); }
    if ($B->a_local()) { print("a_local if T\n"); } else { print("a_local if F\n"); }
    { my @x = (0, $B->a_aref(), 9); print("a_aref item " . scalar(@x) . "\n"); }
    { my @x = $B->a_aref(); print("a_aref arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->a_aref(); print("a_aref destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->a_aref()) { $c++; } print("a_aref foreach " . $c . "\n"); }
    print("a_aref variadic " . cnt($B->a_aref()) . "\n");
    print("a_aref marg " . $B->cnt($B->a_aref()) . "\n");
    { my $ar = [$B->a_aref()]; print("a_aref anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->a_aref()); print("a_aref push " . scalar(@x) . "\n"); }
    { my $n = $B->a_aref(); print("a_aref scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->a_aref(); print("a_aref store " . desc($hh{"n"}) . "\n"); }
    if ($B->a_aref()) { print("a_aref if T\n"); } else { print("a_aref if F\n"); }
    { my @x = (0, $B->hooked(), 9); print("hooked item " . scalar(@x) . "\n"); }
    { my @x = $B->hooked(); print("hooked arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->hooked(); print("hooked destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->hooked()) { $c++; } print("hooked foreach " . $c . "\n"); }
    print("hooked variadic " . cnt($B->hooked()) . "\n");
    print("hooked marg " . $B->cnt($B->hooked()) . "\n");
    { my $ar = [$B->hooked()]; print("hooked anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->hooked()); print("hooked push " . scalar(@x) . "\n"); }
    { my $n = $B->hooked(); print("hooked scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->hooked(); print("hooked store " . desc($hh{"n"}) . "\n"); }
    if ($B->hooked()) { print("hooked if T\n"); } else { print("hooked if F\n"); }
    { my @x = (0, $B->au_list(), 9); print("au_list item " . scalar(@x) . "\n"); }
    { my @x = $B->au_list(); print("au_list arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->au_list(); print("au_list destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->au_list()) { $c++; } print("au_list foreach " . $c . "\n"); }
    print("au_list variadic " . cnt($B->au_list()) . "\n");
    print("au_list marg " . $B->cnt($B->au_list()) . "\n");
    { my $ar = [$B->au_list()]; print("au_list anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->au_list()); print("au_list push " . scalar(@x) . "\n"); }
    { my $n = $B->au_list(); print("au_list scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->au_list(); print("au_list store " . desc($hh{"n"}) . "\n"); }
    if ($B->au_list()) { print("au_list if T\n"); } else { print("au_list if F\n"); }
    { my @x = (0, $B->au_ref(), 9); print("au_ref item " . scalar(@x) . "\n"); }
    { my @x = $B->au_ref(); print("au_ref arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->au_ref(); print("au_ref destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->au_ref()) { $c++; } print("au_ref foreach " . $c . "\n"); }
    print("au_ref variadic " . cnt($B->au_ref()) . "\n");
    print("au_ref marg " . $B->cnt($B->au_ref()) . "\n");
    { my $ar = [$B->au_ref()]; print("au_ref anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->au_ref()); print("au_ref push " . scalar(@x) . "\n"); }
    { my $n = $B->au_ref(); print("au_ref scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->au_ref(); print("au_ref store " . desc($hh{"n"}) . "\n"); }
    if ($B->au_ref()) { print("au_ref if T\n"); } else { print("au_ref if F\n"); }
    { my @x = (0, $CL->(), 9); print("closL item " . scalar(@x) . "\n"); }
    { my @x = $CL->(); print("closL arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $CL->(); print("closL destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($CL->()) { $c++; } print("closL foreach " . $c . "\n"); }
    print("closL variadic " . cnt($CL->()) . "\n");
    print("closL marg " . $B->cnt($CL->()) . "\n");
    { my $ar = [$CL->()]; print("closL anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $CL->()); print("closL push " . scalar(@x) . "\n"); }
    { my $n = $CL->(); print("closL scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $CL->(); print("closL store " . desc($hh{"n"}) . "\n"); }
    if ($CL->()) { print("closL if T\n"); } else { print("closL if F\n"); }
    { my @x = (0, $CS->(), 9); print("closS item " . scalar(@x) . "\n"); }
    { my @x = $CS->(); print("closS arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $CS->(); print("closS destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($CS->()) { $c++; } print("closS foreach " . $c . "\n"); }
    print("closS variadic " . cnt($CS->()) . "\n");
    print("closS marg " . $B->cnt($CS->()) . "\n");
    { my $ar = [$CS->()]; print("closS anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $CS->()); print("closS push " . scalar(@x) . "\n"); }
    { my $n = $CS->(); print("closS scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $CS->(); print("closS store " . desc($hh{"n"}) . "\n"); }
    if ($CS->()) { print("closS if T\n"); } else { print("closS if F\n"); }
    { my @x = (0, $CR->(), 9); print("cref item " . scalar(@x) . "\n"); }
    { my @x = $CR->(); print("cref arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $CR->(); print("cref destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($CR->()) { $c++; } print("cref foreach " . $c . "\n"); }
    print("cref variadic " . cnt($CR->()) . "\n");
    print("cref marg " . $B->cnt($CR->()) . "\n");
    { my $ar = [$CR->()]; print("cref anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $CR->()); print("cref push " . scalar(@x) . "\n"); }
    { my $n = $CR->(); print("cref scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $CR->(); print("cref store " . desc($hh{"n"}) . "\n"); }
    if ($CR->()) { print("cref if T\n"); } else { print("cref if F\n"); }
    { my @x = (0, $B->$MN(), 9); print("dyn item " . scalar(@x) . "\n"); }
    { my @x = $B->$MN(); print("dyn arr " . scalar(@x) . "\n"); }
    { my ($x, $y) = $B->$MN(); print("dyn destr " . desc($x) . "," . desc($y) . "\n"); }
    { my $c = 0; foreach my $e ($B->$MN()) { $c++; } print("dyn foreach " . $c . "\n"); }
    print("dyn variadic " . cnt($B->$MN()) . "\n");
    print("dyn marg " . $B->cnt($B->$MN()) . "\n");
    { my $ar = [$B->$MN()]; print("dyn anon " . scalar(@{$ar}) . "\n"); }
    { my @x = (5); push(@x, $B->$MN()); print("dyn push " . scalar(@x) . "\n"); }
    { my $n = $B->$MN(); print("dyn scalar " . desc($n) . "\n"); }
    { my %hh = (); $hh{"n"} = $B->$MN(); print("dyn store " . desc($hh{"n"}) . "\n"); }
    if ($B->$MN()) { print("dyn if T\n"); } else { print("dyn if F\n"); }
    { my @w = ($B->w_ctx()); my $ws = $B->w_ctx(); print("want " . $w[0] . " " . $ws . "\n"); }
    print("G " . scalar(@G) . " items " . scalar(@{$B->{"items"}}) . "\n");
}
