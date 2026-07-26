#!/usr/bin/env perl
# Feed a fixed input script to ./build/textworld on stdout, pausing before one
# designated line so the session contains a player-scale idle gap (the
# REQ-LAT-9 reconnect path). Used instead of a plain `cat` so the gap is part
# of the recorded run rather than a manual step.
#
#   drive.pl <script> [pause-before-line] [seconds] [max-lines]
use strict;
use warnings;

my ($script, $pauseLine, $seconds, $maxLines) = @ARGV;
$pauseLine //= 0;
$seconds   //= 0;
$maxLines  //= 0;

$| = 1;
open(my $fh, '<', $script) or die "cannot open $script: $!";
my $n = 0;
while (my $line = <$fh>) {
    $n++;
    last if $maxLines && $n > $maxLines;
    select(undef, undef, undef, $seconds) if $pauseLine && $n == $pauseLine;
    print $line;
}
close $fh;
