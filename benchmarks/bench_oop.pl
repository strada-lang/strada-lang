#!/usr/bin/env perl
use strict;
use warnings;

package Point;

sub new {
    my ($class, %args) = @_;
    return bless { x => $args{x} // 0, y => $args{y} // 0 }, $class;
}

sub x { return $_[0]->{x}; }
sub y { return $_[0]->{y}; }

sub distance_sq {
    my ($self) = @_;
    return $self->{x} * $self->{x} + $self->{y} * $self->{y};
}

package Point3D;
our @ISA = ('Point');

sub new {
    my ($class, %args) = @_;
    my $self = Point::new($class, %args);
    $self->{z} = $args{z} // 0;
    return $self;
}

sub z { return $_[0]->{z}; }

sub distance_sq {
    my ($self) = @_;
    return $self->{x} * $self->{x} + $self->{y} * $self->{y} + $self->{z} * $self->{z};
}

package main;

# Create 500,000 Point objects, call method on each
my $sum = 0;
for my $i (0 .. 499_999) {
    my $p = Point->new(x => $i % 100, y => ($i + 1) % 100);
    $sum += $p->distance_sq();
}
print "point sum: $sum\n";

# Create 500,000 Point3D objects (inherited), call overridden method
my $sum3d = 0;
for my $j (0 .. 499_999) {
    my $p = Point3D->new(x => $j % 100, y => ($j + 1) % 100, z => ($j + 2) % 100);
    $sum3d += $p->distance_sq();
}
print "point3d sum: $sum3d\n";

# isa() checks on 200,000 objects
my $isa_count = 0;
for my $k (0 .. 199_999) {
    my $p = Point3D->new(x => 1, y => 2, z => 3);
    if ($p->isa("Point")) {
        $isa_count++;
    }
}
print "isa checks: $isa_count\n";
