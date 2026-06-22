#!/opt/bzperl/bin/perl
# Differential-test reference generator. For every <case>.tt in the corpus dir,
# render it through real Perl Template::Toolkit with vars from <case>.json (if
# present) and write the golden output to <case>.expected. The Strada engine is
# then asserted byte-identical to these files (see run.strada / run_diff.sh).
use strict;
use warnings;
use Template;
use JSON::PP qw(decode_json);

my $dir = $ARGV[0] // 't/template/cases';
opendir(my $dh, $dir) or die "opendir $dir: $!";
my @cases = sort grep { /\.tt$/ } readdir($dh);
closedir($dh);

for my $f (@cases) {
    (my $base = $f) =~ s/\.tt$//;
    my $vars = {};
    my $jf = "$dir/$base.json";
    if (-f $jf) {
        local $/;
        open(my $j, '<', $jf) or die "open $jf: $!";
        my $txt = <$j>;
        close $j;
        $vars = decode_json($txt) if length($txt);
    }
    # Cases named strict_* run with STRICT; a <base>.cfg JSON file merges extra
    # engine config (TAG_STYLE, PRE_CHOMP, ...). Both apply in both engines.
    my %cfg = ( INCLUDE_PATH => $dir );
    $cfg{STRICT} = 1 if $base =~ /^strict_/;
    my $cf = "$dir/$base.cfg";
    if (-f $cf) {
        local $/;
        open(my $c, '<', $cf) or die "open $cf: $!";
        my $cj = <$c>;
        close $c;
        my $extra = decode_json($cj);
        %cfg = (%cfg, %$extra);
    }
    my $tt  = Template->new(\%cfg);
    my $out = '';
    unless ($tt->process($f, $vars, \$out)) {
        $out = "TT-ERROR: " . $tt->error;
    }
    open(my $o, '>', "$dir/$base.expected") or die "write $base.expected: $!";
    print $o $out;
    close $o;
}
print "generated " . scalar(@cases) . " expected output(s)\n";
