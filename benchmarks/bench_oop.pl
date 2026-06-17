#!/usr/bin/perl
use strict;
use warnings;

package Point;

sub new {
    my ($class, %args) = @_;
    return bless {
        x => $args{x} // 0,
        y => $args{y} // 0,
    }, $class;
}

sub x { return $_[0]->{x} }
sub y { return $_[0]->{y} }
sub set_x { $_[0]->{x} = $_[1] }
sub set_y { $_[0]->{y} = $_[1] }

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

sub z { return $_[0]->{z} }
sub set_z { $_[0]->{z} = $_[1] }

sub distance_sq {
    my ($self) = @_;
    return $self->{x} * $self->{x} + $self->{y} * $self->{y} + $self->{z} * $self->{z};
}

package main;

# Create 500,000 Point objects, call method on each
my $sum = 0;
my $i = 0;
while ($i < 500000) {
    my $p = Point->new(x => $i % 100, y => ($i + 1) % 100);
    $sum += $p->distance_sq();
    $i++;
}
print "point sum: $sum\n";

# Create 500,000 Point3D objects (inherited), call overridden method
my $sum3d = 0;
my $j = 0;
while ($j < 500000) {
    my $p = Point3D->new(x => $j % 100, y => ($j + 1) % 100, z => ($j + 2) % 100);
    $sum3d += $p->distance_sq();
    $j++;
}
print "point3d sum: $sum3d\n";

# isa() checks on 200,000 objects
my $isa_count = 0;
my $k = 0;
while ($k < 200000) {
    my $p = Point3D->new(x => 1, y => 2, z => 3);
    if ($p->isa("Point")) {
        $isa_count++;
    }
    $k++;
}
print "isa checks: $isa_count\n";
