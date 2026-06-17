#!/usr/bin/env perl
# Perl counterpart of bench_binary.strada (identical workload).
use strict; use warnings; use Time::HiRes qw(time);
use MIME::Base64 qw(encode_base64 decode_base64);

my $t0 = time;
my @packed;
push @packed, pack("NnC", $_ * 13, $_ % 65536, $_ % 256) . "payload!" for 0..199999;
my $t1 = time;
printf "pack: %d %.6f\n", scalar(@packed), $t1 - $t0;

my $usum = 0;
for my $p (@packed) { my @f = unpack("NnC", $p); $usum += $f[1] }
my $t2 = time;
printf "unpack: %d %.6f\n", $usum, $t2 - $t1;

my $blob = "";
$blob .= pack("CCCC", $_ % 256, ($_ * 7) % 256, ($_ * 13) % 256, ($_ * 31) % 256) for 0..65535;
my $b64_len = 0;
for (1..20) {
    my $enc = encode_base64($blob, "");
    my $dec = decode_base64($enc);
    $b64_len += length $dec;
}
my $t3 = time;
printf "base64: %d %.6f\n", $b64_len, $t3 - $t2;

my $big = $blob x 8;
my ($cksum, $blen) = (0, length $big);
for (my $i = 0; $i < $blen; $i += 16) { $cksum = ($cksum + ord(substr($big, $i, 1))) % 65536 }
my $t4 = time;
printf "bytes: %d %.6f\n", $cksum, $t4 - $t3;

my $frame = "";
$frame .= chr($_ % 256) for 0..999999;
my $t5 = time;
printf "build: %d %.6f\n", length($frame), $t5 - $t4;
printf "total: %.6f\n", $t5 - $t0;
