# Generates forms.strada and forms.pl: the same table of (return form x context).
forms = [  # name, strada body, perl body
 ("local",  'my array @p = (1, 2, 3); return @p;',            'my @p = (1, 2, 3); return @p;'),
 ("our",    'return @G;',                                     'return @G;'),
 ("deref",  'return @{$R};',                                  'return @{$R};'),
 ("comma",  'return (7, 8, 9);',                              'return (7, 8, 9);'),
 ("two",    'return (@A2, @B2);',                             'return (@A2, @B2);'),
 ("empty",  'return ();',                                     'return ();'),
 ("bare",   'return;',                                        'return;'),
 ("aref",   'return [1, 2, 3];',                              'return [1, 2, 3];'),
 ("refto",  'return \\@G;',                                   'return \\@G;'),
 ("sref",   'my scalar $r = [1, 2, 3]; return $r;',           'my $r = [1, 2, 3]; return $r;'),
 ("elem",   'return $H{"k"};',                                'return $H{"k"};'),
 ("elemv",  'return $H{"v"};',                                'return $H{"v"};'),
 ("href",   'return {"a" => 1};',                             'return {"a" => 1};'),
 ("map",    'return map { $_ * 2 } @G;',                      'return map { $_ * 2 } @G;'),
 ("ternL",  'return $T ? @G : [9];',                          'return $T ? @G : [9];'),
 ("ternS",  'return $F ? @G : [9];',                          'return $F ? @G : [9];'),
 ("call",   'return f_local();',                              'return f_local();'),
 ("callS",  'return f_aref();',                               'return f_aref();'),
 ("scal",   'return 5;',                                      'return 5;'),
 ("str",    'return "s";',                                    'return "s";'),
]
S=[]; P=[]
S.append('''our array @G = (1, 2, 3);
our scalar $R = [4, 5];
our array @A2 = (1, 2);
our array @B2 = (3);
our hash %H = ();
our int $T = 1;
our int $F = 0;
func desc(scalar $v) str {
    if (!defined($v)) { return "u"; }
    if (length(ref($v)) > 0) { return ref($v); }
    return "" . $v;
}
func cnt(scalar ...@r) int { return scalar(@r); }
package Box;
func new() scalar { my hash %s = ("items" => [1, 2, 3]); return bless(\\%s, "Box"); }
func cnt { my $self = shift; return scalar(@_); }
func m_local(scalar $self) scalar { my array @p = (1, 2, 3); return @p; }
func m_aref(scalar $self) scalar { return [1, 2, 3]; }
func m_items(scalar $self) scalar { return $self->{"items"}; }
func m_all(scalar $self) scalar { return @{$self->{"items"}}; }
func m_arr(scalar $self) array { return (1, 2, 3); }
func a_local { my $self = shift; my array @p = (1, 2, 3); return @p; }
func a_aref { my $self = shift; return [1, 2, 3]; }
func w_ctx { my $self = shift; return core::wantarray() ? "list" : "scalar"; }
func hooked(scalar $self) scalar { return (1, 2, 3); }
around "hooked" func(scalar $orig, scalar $self) scalar { return $orig->($self); }
func AUTOLOAD(scalar $self, str $m, scalar ...@r) scalar { if ($m eq "au_list") { return (1, 2, 3); } return [1, 2, 3]; }
package main;
''')
P.append('''use strict; use warnings; no warnings qw(once);
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
''')
for n,sb,pb in forms:
    S.append(f'func f_{n}() {{ {sb} }}\n')
    P.append(f'sub f_{n} {{ {pb} }}\n')
def rows(label, scall, pcall):
    # contexts; each prints "label ctx value"
    s=[];p=[]
    s.append(f'    {{ my array @x = (0, {scall}, 9); say("{label} item " . scalar(@x)); }}')
    p.append(f'    {{ my @x = (0, {pcall}, 9); print("{label} item " . scalar(@x) . "\\n"); }}')
    s.append(f'    {{ my array @x = {scall}; say("{label} arr " . scalar(@x)); }}')
    p.append(f'    {{ my @x = {pcall}; print("{label} arr " . scalar(@x) . "\\n"); }}')
    s.append(f'    {{ my ($x, $y) = {scall}; say("{label} destr " . desc($x) . "," . desc($y)); }}')
    p.append(f'    {{ my ($x, $y) = {pcall}; print("{label} destr " . desc($x) . "," . desc($y) . "\\n"); }}')
    s.append(f'    {{ my int $c = 0; foreach my $e ({scall}) {{ $c++; }} say("{label} foreach " . $c); }}')
    p.append(f'    {{ my $c = 0; foreach my $e ({pcall}) {{ $c++; }} print("{label} foreach " . $c . "\\n"); }}')
    s.append(f'    say("{label} variadic " . cnt({scall}));')
    p.append(f'    print("{label} variadic " . cnt({pcall}) . "\\n");')
    s.append(f'    say("{label} marg " . $B->cnt({scall}));')
    p.append(f'    print("{label} marg " . $B->cnt({pcall}) . "\\n");')
    s.append(f'    {{ my scalar $ar = [{scall}]; say("{label} anon " . scalar(@{{$ar}})); }}')
    p.append(f'    {{ my $ar = [{pcall}]; print("{label} anon " . scalar(@{{$ar}}) . "\\n"); }}')
    s.append(f'    {{ my array @x = (5); push(@x, {scall}); say("{label} push " . scalar(@x)); }}')
    p.append(f'    {{ my @x = (5); push(@x, {pcall}); print("{label} push " . scalar(@x) . "\\n"); }}')
    s.append(f'    {{ my $n = {scall}; say("{label} scalar " . desc($n)); }}')
    p.append(f'    {{ my $n = {pcall}; print("{label} scalar " . desc($n) . "\\n"); }}')
    s.append(f'    {{ my hash %hh = (); $hh{{"n"}} = {scall}; say("{label} store " . desc($hh{{"n"}})); }}')
    p.append(f'    {{ my %hh = (); $hh{{"n"}} = {pcall}; print("{label} store " . desc($hh{{"n"}}) . "\\n"); }}')
    s.append(f'    if ({scall}) {{ say("{label} if T"); }} else {{ say("{label} if F"); }}')
    p.append(f'    if ({pcall}) {{ print("{label} if T\\n"); }} else {{ print("{label} if F\\n"); }}')
    return s,p
S.append('func main() int {\n    my scalar $B = Box::new();\n    $H{"k"} = [1, 2];\n    push(@{$H{"v"}}, 1); push(@{$H{"v"}}, 2);\n    foreach my int $round (1 .. 2) {\n')
P.append('my $B = Box->new;\n$H{"k"} = [1, 2];\npush(@{$H{"v"}}, 1); push(@{$H{"v"}}, 2);\nfor my $round (1 .. 2) {\n')
calls=[(f"f_{n}", f"f_{n}()", f"f_{n}()") for n,_,_ in forms]
for m in ["m_local","m_aref","m_items","m_all","m_arr","a_local","a_aref","hooked","au_list","au_ref"]:
    calls.append((m, f"$B->{m}()", f"$B->{m}()"))
calls.append(("closL", "$CL->()", "$CL->()")); calls.append(("closS", "$CS->()", "$CS->()"))
calls.append(("cref", "$CR->()", "$CR->()"))
calls.append(("dyn", "$B->$MN()", "$B->$MN()"))
S.append('    my scalar $CL = func () { my array @p = (1, 2, 3); return @p; };\n    my scalar $CS = func () { return [1, 2, 3]; };\n    my scalar $CR = \\&f_local;\n    my str $MN = "m_local";\n')
P.append('    my $CL = sub { my @p = (1, 2, 3); return @p; };\n    my $CS = sub { return [1, 2, 3]; };\n    my $CR = \\&f_local;\n    my $MN = "m_local";\n')
for label,sc,pc in calls:
    s,p=rows(label,sc,pc); S.append('\n'.join(s)+'\n'); P.append('\n'.join(p)+'\n')
# wantarray
S.append('    { my array @w = ($B->w_ctx()); my $ws = $B->w_ctx(); say("want " . $w[0] . " " . $ws); }\n')
P.append('    { my @w = ($B->w_ctx()); my $ws = $B->w_ctx(); print("want " . $w[0] . " " . $ws . "\\n"); }\n')
S.append('    say("G " . scalar(@G) . " items " . scalar(@{$B->{"items"}}));\n    }\n    return 0;\n}\n')
P.append('    print("G " . scalar(@G) . " items " . scalar(@{$B->{"items"}}) . "\\n");\n}\n')
open('forms.strada','w').write(''.join(S)); open('forms.pl','w').write(''.join(P))
