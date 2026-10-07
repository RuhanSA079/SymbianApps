#!/usr/bin/perl
# Make the SDK's 2010-era Perl build scripts run on Perl >= 5.22, where
# defined(@array) / defined(%hash) became a fatal error.
#   defined @x,  defined(%x)          ->  (@x), (%x)
#   defined @{E}, defined(%{E})       ->  ((E) && @{E})   (no strict-refs death on undef)
# Usage: fix-perl.pl FILE...   (edits in place, prints the files it changed)
use strict;
use warnings;

my $brace = qr/(\{(?:[^{}]++|(?-1))*\})/;    # balanced {...}

sub deref { my ($sig, $b) = @_; my $e = substr($b, 1, -1); "(($e) && $sig\{$e\})" }

for my $f (@ARGV) {
    open my $in, '<', $f or die "$f: $!";
    my $src = do { local $/; <$in> };
    close $in;
    my $n = 0;
    $n += $src =~ s/\bdefined\s*\(\s*([%@])$brace\s*\)/deref($1, $2)/ge;
    $n += $src =~ s/\bdefined\s+([%@])$brace/deref($1, $2)/ge;
    $n += $src =~ s/\bdefined\s*\(\s*([%@][\w:]+)\s*\)/($1)/g;
    $n += $src =~ s/\bdefined\s+([%@][\w:]+)/($1)/g;
    next unless $n;
    open my $out, '>', $f or die "$f: $!";
    print $out $src;
    close $out;
    print "fixed $n in $f\n";
}
