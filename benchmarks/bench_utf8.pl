#!/usr/bin/env perl
# Perl counterpart of bench_utf8.strada (decoded strings + Encode/
# Unicode::Normalize; equivalent user-visible work, different model:
# Perl operates on decoded codepoints, Strada on UTF-8 bytes).
use strict; use warnings; use utf8;
use Time::HiRes qw(time);
use Encode qw(encode decode FB_CROAK);
use Unicode::Normalize qw(NFC);

my @corpus;
push @corpus, "Müller-Straße $_ — café №" . ($_ % 100) . " übergröße" for 0..199999;
my @raw = map { encode('UTF-8', $_) } @corpus[0..199999];

my $t0 = time;
my $case_len = 0;
for my $s (@corpus) { my $u = uc $s; my $l = lc $u; $case_len += length(encode('UTF-8', $l)) }
my $t1 = time;
printf "case: %d %.6f\n", $case_len, $t1 - $t0;

my $valid = 0;
for my $b (@raw) { $valid++ if eval { decode('UTF-8', $b, FB_CROAK); 1 } }
my $t2 = time;
printf "valid: %d %.6f\n", $valid, $t2 - $t1;

my $built = 0;
for my $i (0..49999) {
    my $s = "";
    for my $j (0..19) { $s .= chr(0xE9 + ($j % 16)); $s .= chr(0x4E00 + ($j % 64)) }
    $built += length(encode('UTF-8', $s));
}
my $t3 = time;
printf "chr-build: %d %.6f\n", $built, $t3 - $t2;

my $acc = "";
$acc .= "ü" . $_ . "ß—" for 0..29999;
my $t4 = time;
printf "concat: %d %.6f\n", length(encode('UTF-8', $acc)), $t4 - $t3;

my $decomposed = "Cafe\x{301} resume\x{301} naive\x{308}";
my $nfc_len = 0;
$nfc_len += length(encode('UTF-8', NFC($decomposed))) for 1..50000;
my $t5 = time;
printf "nfc: %d %.6f\n", $nfc_len, $t5 - $t4;
printf "total: %.6f\n", $t5 - $t0;
