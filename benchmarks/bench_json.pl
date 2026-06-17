#!/usr/bin/env perl
# Perl counterpart of bench_json.strada (JSON::PP — core, pure-Perl, the
# fair comparison against Strada's pure-Strada JSON module).
use strict; use warnings; use Time::HiRes qw(time);
use JSON::PP;

my $json_codec = JSON::PP->new;

sub build_doc {
    my $users = shift;
    my @list;
    for my $i (0..$users-1) {
        push @list, {
            id => $i, name => "user_$i", email => "user$i\@example.com",
            active => ($i % 3) == 0 ? 1 : 0, score => $i * 1.5,
            tags => ["alpha", "beta", "tag" . ($i % 50)],
            profile => { city => "city" . ($i % 100), zip => 10000 + ($i % 90000), langs => ["en", "de"] },
        };
    }
    return { count => $users, users => \@list };
}

my $doc = build_doc(2000);
my $t0 = time;
my ($enc_len, $json) = (0, "");
for (1..20) { $json = $json_codec->encode($doc); $enc_len += length $json }
my $t1 = time;
printf "encode: %d %.6f\n", $enc_len, $t1 - $t0;

my $dec_users = 0;
for (1..20) { my $p = $json_codec->decode($json); $dec_users += $p->{count} }
my $t2 = time;
printf "decode: %d %.6f\n", $dec_users, $t2 - $t1;

my $rt = 0;
for my $i (0..19999) {
    my $small = $json_codec->encode({ op => "get", id => $i, args => [1,2,3] });
    my $back = $json_codec->decode($small);
    $rt += $back->{id} % 2;
}
my $t3 = time;
printf "roundtrip: %d %.6f\n", $rt, $t3 - $t2;
printf "total: %.6f\n", $t3 - $t0;
