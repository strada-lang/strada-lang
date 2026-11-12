# Structs in Strada

Structs provide a way to group related data together, similar to C structs.

> **Status (current compiler):** Strada has **no working struct feature**.
>
> - **Native struct syntax is not supported.** `struct Point { int x; int y; }`
>   is rejected by the parser (`unexpected 'struct' at top level`), as are
>   struct-typed variables (`my Point $pt;` — `expected a type (int, num, str,
>   array, hash, scalar), got 'Point'`). Field assignment without parentheses
>   (`$pt->x = 10;`) fails at the C-compile step.
> - **The `core::cstruct_*` functions do not work.** They compile and link, but
>   `core::cstruct_new($size)` returns an ordinary empty hash, so
>   `core::cstruct_set_int/double/string` store nothing and
>   `core::cstruct_get_int` always returns 0.
>
> Use one of the approaches below instead: a class with `has` attributes
> (fixed, named, typed fields), a plain hash, or — for real C memory layouts —
> the `c::` raw-memory functions. Every example on this page has been compiled
> and run against the current compiler.

## Fixed Fields: a Class with `has`

The closest equivalent to a struct is a package with `has` attributes. Each
attribute gets a typed accessor (`$pt->x()`), `rw` attributes also get a setter
(`$pt->set_x(...)`), and the compiler generates a constructor taking
name/value pairs.

### Defining the Type

```strada
package Point;
has rw int $x = 0;
has rw int $y = 0;

package Person;
has rw str $name = "";
has rw int $age = 0;
has rw num $salary = 0.0;
```

### Creating Values and Accessing Fields

```strada
my scalar $pt = Point::new("x", 10, "y", 20);   # name, value, name, value, ...

# Set fields
$pt->set_x(15);
$pt->set_y(25);

# Read fields
say("x = " . $pt->x());
say("y = " . $pt->y());
```

Pass the constructor pairs separated by commas (`"x", 10`) or with the fat
comma (`Point::new(x => 10, y => 20)`); in a call argument list `=>` is just a
comma.

### Functions and Methods

Functions in the package that take `$self` first are methods:

```strada
package Point;
has rw int $x = 0;
has rw int $y = 0;

func move(scalar $self, int $dx, int $dy) void {
    $self->set_x($self->x() + $dx);
    $self->set_y($self->y() + $dy);
}

func to_text(scalar $self) str {
    return "Point(" . $self->x() . ", " . $self->y() . ")";
}

func distance(scalar $self, scalar $other) num {
    my int $dx = $other->x() - $self->x();
    my int $dy = $other->y() - $self->y();
    return math::sqrt($dx * $dx + $dy * $dy);
}
```

### Complete Example

```strada
package Rectangle;
has rw int $x = 0;
has rw int $y = 0;
has rw int $width = 0;
has rw int $height = 0;

func area(scalar $self) int {
    return $self->width() * $self->height();
}

package main;

func main() int {
    my scalar $box = Rectangle::new("width", 100, "height", 50);
    say("Area: " . $box->area());  # Area: 5000
    return 0;
}
```

## Raw C Memory Layouts: the `c::` Functions

When you need a real C struct layout (to hand a pointer to C code, or to read
one back), allocate raw memory and read/write fields at explicit byte offsets.

```strada
# struct point { int32_t x; int32_t y; double weight; }  -> 16 bytes
# x: int32 at offset 0, y: int32 at offset 4, weight: double at offset 8
func point_new(int $x, int $y, num $weight) scalar {
    my scalar $p = c::alloc(16);
    c::write_int32($p, $x);
    c::write_int32(c::ptr_add($p, 4), $y);
    c::write_double(c::ptr_add($p, 8), $weight);
    return $p;
}

func point_x(scalar $p) int { return c::read_int32($p); }
func point_y(scalar $p) int { return c::read_int32(c::ptr_add($p, 4)); }
func point_weight(scalar $p) num { return c::read_double(c::ptr_add($p, 8)); }

func main() int {
    my scalar $p = point_new(10, 20, 1.5);
    say(point_x($p));        # 10
    say(point_y($p));        # 20
    say(point_weight($p));   # 1.5
    c::free($p);             # required: c::alloc memory is not reference counted
    return 0;
}
```

### `c::` Memory API Reference

| Function | Description |
|----------|-------------|
| `c::alloc($size)` | Allocate `$size` bytes; returns a pointer |
| `c::realloc($ptr, $size)` | Resize an allocation |
| `c::free($ptr)` | Free memory from `c::alloc` / `c::str_to_ptr` |
| `c::ptr_add($ptr, $offset)` | Pointer plus byte offset |
| `c::is_null($ptr)`, `c::null()` | NULL test / NULL pointer |
| `c::write_int8/16/32/64($ptr, $val)` | Write an integer of that width |
| `c::read_int8/16/32/64($ptr)` | Read an integer of that width |
| `c::write_float($ptr, $val)`, `c::write_double($ptr, $val)` | Write a float / double |
| `c::read_float($ptr)`, `c::read_double($ptr)` | Read a float / double |
| `c::write_ptr($ptr, $val)`, `c::read_ptr($ptr)` | Write / read a pointer-sized value |
| `c::str_to_ptr($str)` | Copy a string to newly allocated C memory |
| `c::ptr_to_str($ptr)`, `c::ptr_to_str_n($ptr, $len)` | Read a C string back |
| `c::memcpy($dst, $src, $n)`, `c::memset($ptr, $byte, $n)` | Block copy / fill |
| `c::sizeof_int()`, `c::sizeof_long()`, `c::sizeof_ptr()`, `c::sizeof_size_t()` | Platform sizes |

## Classes vs Hashes vs Raw Memory

When should you use each?

### Use a Class (`has`) When:

- You need fixed, known, typed fields
- You want methods, inheritance, or destructors (`DESTROY`)
- Performance matters (accessors and constructors are inlined by the compiler)

```strada
package Point;
has rw int $x = 0;
has rw int $y = 0;

package main;

func main() int {
    my scalar $p = Point::new("x", 10);
    say($p->x());   # 10
    return 0;
}
```

### Use Hashes When:

- Fields are dynamic or unknown at compile time
- Flexibility is more important than speed
- Rapid prototyping

```strada
my hash %point = ("x" => 10, "y" => 20);
$point{"z"} = 30;  # Can add fields anytime
```

### Use a Hand-Written `bless` Class When:

- You want full control over construction and storage

```strada
package Point;
func new(int $x, int $y) scalar {
    return bless({ "x" => $x, "y" => $y }, "Point");
}
func move(scalar $self, int $dx, int $dy) void {
    $self->{"x"} = $self->{"x"} + $dx;
    $self->{"y"} = $self->{"y"} + $dy;
}
```

### Use `c::` Raw Memory When:

- Interfacing with C code
- Memory layout must be predictable

## Common Patterns

### Constructor with Defaults

Attribute defaults replace hand-written initialization:

```strada
package Config;
has rw str $host = "localhost";
has rw int $port = 80;
has rw int $timeout = 30;      # Default value

package main;

func main() int {
    my scalar $c = Config::new("host", "example.com", "port", 8080);
    say($c->host() . ":" . $c->port() . " timeout " . $c->timeout());
    return 0;
}
```

### Array of Objects

```strada
package Point;
has rw int $x = 0;
has rw int $y = 0;

package main;

func main() int {
    my array @points = ();

    push(@points, Point::new("x", 0, "y", 0));
    push(@points, Point::new("x", 10, "y", 20));
    push(@points, Point::new("x", 30, "y", 40));

    foreach my scalar $p (@points) {
        say("(" . $p->x() . ", " . $p->y() . ")");
    }

    return 0;
}
```

### Nested Data (Object + Hash)

```strada
package Employee;
has rw str $name = "";
has rw int $id = 0;

package main;

func main() int {
    my hash %department = ();

    $department{"manager"} = Employee::new("name", "Alice", "id", 101);
    $department{"developer"} = Employee::new("name", "Bob", "id", 102);

    my scalar $mgr = $department{"manager"};
    say("Manager: " . $mgr->name());

    return 0;
}
```

## Memory Considerations

### Objects and Hashes

Objects and hashes are reference counted like other Strada values and are
freed automatically when the last reference goes away.

### `c::alloc` Memory

Memory from `c::alloc` (and `c::str_to_ptr`) is **not** reference counted. The
pointer is just a number; you must call `c::free($ptr)` yourself, exactly once.

## Tips

1. **Prefer a `has` class** - It's cleaner and less error-prone than manual offsets

2. **Document your layouts** - When using `c::` raw memory, comment the layout:
   ```strada
   # point: x(int32,0), y(int32,4), weight(double,8) = 16 bytes total
   ```

3. **Use attribute defaults** - Don't repeat initialization code

4. **Consider alignment** - When interfacing with C, match C struct alignment

## Examples

See these files for more examples:

- `examples/test_c_callback.strada` - `c::alloc` / `c::read_*` / `c::write_*` with C callbacks
- `examples/test_extern_c.strada` - Calling C from Strada
