# math:: Namespace Reference

The `math::` namespace provides mathematical functions for trigonometry,
logarithms, rounding, and other common numeric operations. These functions
wrap the C standard library math functions.

## Basic Arithmetic

### math::abs

```strada
my num $result = math::abs($x);
```

Return the absolute value of a number.

- `$x` - Any numeric value

Returns the absolute value (always non-negative).

```strada
my num $a = math::abs(-5);      # 5
my num $b = math::abs(3.14);    # 3.14
my num $c = math::abs(-2.5);    # 2.5
```

### math::pow

```strada
my num $result = math::pow($base, $exponent);
```

Raise a number to a power.

- `$base` - The base value
- `$exponent` - The exponent (power)

Returns base raised to the exponent power.

```strada
my num $a = math::pow(2, 10);   # 1024
my num $b = math::pow(10, 3);   # 1000
my num $c = math::pow(2, 0.5);  # 1.414... (square root of 2)
```

### math::sqrt

```strada
my num $result = math::sqrt($x);
```

Calculate the square root.

- `$x` - A non-negative number

Returns the square root.

```strada
my num $a = math::sqrt(16);     # 4
my num $b = math::sqrt(2);      # 1.41421356...
my num $c = math::sqrt(100);    # 10
```

### math::fmod

```strada
my num $result = math::fmod($x, $y);
```

Calculate the floating-point remainder of division.

- `$x` - The dividend
- `$y` - The divisor

Returns the remainder of x/y with the same sign as x.

```strada
my num $a = math::fmod(5.3, 2);     # 1.3
my num $b = math::fmod(-5.3, 2);    # -1.3
my num $c = math::fmod(18.5, 4.2);  # 1.7
```

## Rounding Functions

### math::floor

```strada
my num $result = math::floor($x);
```

Round down to the nearest integer (toward negative infinity).

- `$x` - Any numeric value

Returns the largest integer less than or equal to x.

```strada
my num $a = math::floor(3.7);   # 3
my num $b = math::floor(-3.7);  # -4
my num $c = math::floor(5.0);   # 5
```

### math::ceil

```strada
my num $result = math::ceil($x);
```

Round up to the nearest integer (toward positive infinity).

- `$x` - Any numeric value

Returns the smallest integer greater than or equal to x.

```strada
my num $a = math::ceil(3.2);    # 4
my num $b = math::ceil(-3.2);   # -3
my num $c = math::ceil(5.0);    # 5
```

### math::round

```strada
my num $result = math::round($x);
```

Round to the nearest integer (half away from zero).

- `$x` - Any numeric value

Returns the nearest integer.

```strada
my num $a = math::round(3.4);   # 3
my num $b = math::round(3.5);   # 4
my num $c = math::round(-3.5);  # -4
```

## Trigonometric Functions

All trigonometric functions work in **radians**, not degrees.

To convert degrees to radians: `$radians = $degrees * 3.14159265359 / 180`

To convert radians to degrees: `$degrees = $radians * 180 / 3.14159265359`

### math::sin

```strada
my num $result = math::sin($x);
```

Calculate the sine of an angle.

- `$x` - Angle in radians

Returns the sine (range: -1 to 1).

```strada
my num $a = math::sin(0);           # 0
my num $b = math::sin(1.5707963);   # ~1 (pi/2)
my num $c = math::sin(3.1415926);   # ~0 (pi)
```

### math::cos

```strada
my num $result = math::cos($x);
```

Calculate the cosine of an angle.

- `$x` - Angle in radians

Returns the cosine (range: -1 to 1).

```strada
my num $a = math::cos(0);           # 1
my num $b = math::cos(1.5707963);   # ~0 (pi/2)
my num $c = math::cos(3.1415926);   # ~-1 (pi)
```

### math::tan

```strada
my num $result = math::tan($x);
```

Calculate the tangent of an angle.

- `$x` - Angle in radians

Returns the tangent.

```strada
my num $a = math::tan(0);           # 0
my num $b = math::tan(0.7853981);   # ~1 (pi/4 = 45 degrees)
```

## Inverse Trigonometric Functions

### math::asin

```strada
my num $result = math::asin($x);
```

Calculate the arc sine (inverse sine).

- `$x` - Value in range -1 to 1

Returns the angle in radians (range: -pi/2 to pi/2).

```strada
my num $a = math::asin(0);    # 0
my num $b = math::asin(1);    # 1.5707963... (pi/2)
my num $c = math::asin(0.5);  # 0.5235987... (pi/6)
```

### math::acos

```strada
my num $result = math::acos($x);
```

Calculate the arc cosine (inverse cosine).

- `$x` - Value in range -1 to 1

Returns the angle in radians (range: 0 to pi).

```strada
my num $a = math::acos(1);    # 0
my num $b = math::acos(0);    # 1.5707963... (pi/2)
my num $c = math::acos(-1);   # 3.1415926... (pi)
```

### math::atan

```strada
my num $result = math::atan($x);
```

Calculate the arc tangent (inverse tangent).

- `$x` - Any numeric value

Returns the angle in radians (range: -pi/2 to pi/2).

```strada
my num $a = math::atan(0);    # 0
my num $b = math::atan(1);    # 0.7853981... (pi/4)
my num $c = math::atan(-1);   # -0.7853981... (-pi/4)
```

### math::atan2

```strada
my num $result = math::atan2($y, $x);
```

Calculate the arc tangent of y/x, using the signs of both arguments
to determine the quadrant. This is the preferred way to convert
Cartesian coordinates to polar coordinates.

- `$y` - The y coordinate
- `$x` - The x coordinate

Returns the angle in radians (range: -pi to pi).

```strada
my num $a = math::atan2(1, 1);    # 0.7853981... (pi/4, 45 degrees)
my num $b = math::atan2(1, -1);   # 2.3561944... (3*pi/4, 135 degrees)
my num $c = math::atan2(-1, -1);  # -2.3561944... (-3*pi/4, -135 degrees)
```

## Hyperbolic Functions

### math::sinh

```strada
my num $result = math::sinh($x);
```

Calculate the hyperbolic sine.

- `$x` - Any numeric value

```strada
my num $a = math::sinh(0);    # 0
my num $b = math::sinh(1);    # 1.1752011...
```

### math::cosh

```strada
my num $result = math::cosh($x);
```

Calculate the hyperbolic cosine.

- `$x` - Any numeric value

```strada
my num $a = math::cosh(0);    # 1
my num $b = math::cosh(1);    # 1.5430806...
```

### math::tanh

```strada
my num $result = math::tanh($x);
```

Calculate the hyperbolic tangent.

- `$x` - Any numeric value

Returns a value in the range -1 to 1.

```strada
my num $a = math::tanh(0);    # 0
my num $b = math::tanh(1);    # 0.7615941...
my num $c = math::tanh(100);  # ~1 (approaches 1 for large values)
```

## Exponential and Logarithmic Functions

### math::exp

```strada
my num $result = math::exp($x);
```

Calculate e raised to the power x (the exponential function).

- `$x` - The exponent

Returns e^x where e is Euler's number (~2.71828).

```strada
my num $a = math::exp(0);     # 1
my num $b = math::exp(1);     # 2.7182818... (e)
my num $c = math::exp(2);     # 7.3890560... (e^2)
```

### math::log

```strada
my num $result = math::log($x);
```

Calculate the natural logarithm (base e).

- `$x` - A positive number

Returns the natural log of x.

```strada
my num $a = math::log(1);         # 0
my num $b = math::log(2.7182818); # ~1 (ln(e))
my num $c = math::log(10);        # 2.3025850...
```

### math::log10

```strada
my num $result = math::log10($x);
```

Calculate the base-10 logarithm.

- `$x` - A positive number

Returns the log base 10 of x.

```strada
my num $a = math::log10(1);     # 0
my num $b = math::log10(10);    # 1
my num $c = math::log10(100);   # 2
my num $d = math::log10(1000);  # 3
```

## Special Value Testing

### math::isnan

```strada
my int $result = math::isnan($x);
```

Test if a value is NaN (Not a Number).

- `$x` - Any numeric value

Returns 1 if x is NaN, 0 otherwise.

```strada
my num $nan = math::sqrt(-1);   # NaN
my int $a = math::isnan($nan);  # 1
my int $b = math::isnan(5.0);   # 0
```

### math::isinf

```strada
my int $result = math::isinf($x);
```

Test if a value is infinite.

- `$x` - Any numeric value

Returns 1 if x is positive infinity, -1 if negative infinity, 0 otherwise.

```strada
my num $inf = math::pow(10, 1000);  # Infinity
my int $a = math::isinf($inf);      # 1
my int $b = math::isinf(5.0);       # 0
```

## Random Numbers

### math::rand

```strada
my num $result = math::rand();
```

Generate a random floating-point number between 0 and 1.

Returns a pseudo-random number in the range [0, 1).

```strada
my num $r = math::rand();       # e.g., 0.7234...
my int $die = int(math::rand() * 6) + 1;  # Random 1-6
```

### math::srand

```strada
math::srand($seed);
```

Seed the random number generator.

- `$seed` - An integer seed value

Use to get reproducible sequences of random numbers.

```strada
math::srand(12345);             # Set seed
my num $r1 = math::rand();      # Will always be the same
my num $r2 = math::rand();      # for this seed

math::srand(core::time());       # Seed with current time
```

## Constants

Strada does not have built-in math constants, but you can define them:

```strada
my num $PI = 3.14159265358979323846;
my num $E = 2.71828182845904523536;
my num $SQRT2 = 1.41421356237309504880;
my num $LN2 = 0.69314718055994530942;
my num $LN10 = 2.30258509299404568402;
```

## Examples

### Calculate distance between two points

```strada
func distance(num $x1, num $y1, num $x2, num $y2) num {
    my num $dx = $x2 - $x1;
    my num $dy = $y2 - $y1;
    return math::sqrt($dx * $dx + $dy * $dy);
}

my num $d = distance(0, 0, 3, 4);  # 5
```

### Convert polar to Cartesian coordinates

```strada
func polar_to_cartesian(num $r, num $theta) array {
    my num $x = $r * math::cos($theta);
    my num $y = $r * math::sin($theta);
    return ($x, $y);
}
```

### Calculate compound interest

```strada
func compound_interest(num $principal, num $rate, int $years) num {
    return $principal * math::pow(1 + $rate, $years);
}

my num $amount = compound_interest(1000, 0.05, 10);  # ~1628.89
```

### Generate random integers in a range

```strada
func rand_range(int $min, int $max) int {
    return int(math::rand() * ($max - $min + 1)) + $min;
}

my int $roll = rand_range(1, 20);  # D&D d20 roll
```

### Normalize an angle to 0-2*PI

```strada
func normalize_angle(num $angle) num {
    my num $PI = 3.14159265358979;
    my num $TWO_PI = $PI * 2;
    while ($angle < 0) {
        $angle = $angle + $TWO_PI;
    }
    while ($angle >= $TWO_PI) {
        $angle = $angle - $TWO_PI;
    }
    return $angle;
}
```
