# Structs in Strada

Structs provide a way to group related data together, similar to C structs.
Strada offers two approaches: native struct syntax and the low-level cstruct API.

## Native Struct Syntax

The preferred way to use structs in Strada.

### Defining a Struct

```strada
struct Point {
    int x;
    int y;
}

struct Person {
    str name;
    int age;
    num salary;
}
```

### Declaring Struct Variables

```strada
my Point $pt;
my Person $alice;
```

### Accessing Fields

Use the arrow operator `->` to read and write fields:

```strada
# Set fields
$pt->x = 10;
$pt->y = 20;

# Read fields
say("x = " . $pt->x);
say("y = " . $pt->y);
```

### Structs in Functions

Functions can take and return structs:

```strada
func create_point(int $x, int $y) Point {
    my Point $pt;
    $pt->x = $x;
    $pt->y = $y;
    return $pt;
}

func print_point(Point $pt) void {
    say("Point(" . $pt->x . ", " . $pt->y . ")");
}

func move_point(Point $pt, int $dx, int $dy) Point {
    $pt->x = $pt->x + $dx;
    $pt->y = $pt->y + $dy;
    return $pt;
}
```

### Complete Example

```strada
struct Rectangle {
    int x;
    int y;
    int width;
    int height;
}

func create_rect(int $x, int $y, int $w, int $h) Rectangle {
    my Rectangle $r;
    $r->x = $x;
    $r->y = $y;
    $r->width = $w;
    $r->height = $h;
    return $r;
}

func area(Rectangle $r) int {
    return $r->width * $r->height;
}

func main() int {
    my Rectangle $box = create_rect(0, 0, 100, 50);
    say("Area: " . area($box));  # Area: 5000
    return 0;
}
```

### Supported Field Types

| Type | Description |
|------|-------------|
| `int` | Integer (4 bytes) |
| `num` | Float/double (8 bytes) |
| `str` | String |

## Low-Level cstruct API

For more control or C interop, use the cstruct functions directly.

### Creating a Struct

Allocate a block of memory:

```strada
# Allocate 8 bytes (for two 4-byte ints)
my scalar $pt = sys::cstruct_new(8);
```

### Setting Fields

Specify the field name, byte offset, and value:

```strada
# Integer field (4 bytes)
sys::cstruct_set_int($pt, "x", 0, 10);    # x at offset 0
sys::cstruct_set_int($pt, "y", 4, 20);    # y at offset 4

# Double field (8 bytes)
sys::cstruct_set_double($p, "salary", 68, 50000.0);

# String field (fixed size)
sys::cstruct_set_string($p, "name", 0, "Alice");
```

### Getting Fields

```strada
my int $x = sys::cstruct_get_int($pt, "x", 0);
my int $y = sys::cstruct_get_int($pt, "y", 4);
my num $salary = sys::cstruct_get_double($p, "salary", 68);
my str $name = sys::cstruct_get_string($p, "name", 0);
```

### Memory Layout Example

```strada
# Person struct layout:
#   name:   string, 64 bytes, offset 0
#   age:    int, 4 bytes, offset 64
#   salary: double, 8 bytes, offset 68
# Total: 76 bytes

func create_person(str $name, int $age, num $salary) scalar {
    my scalar $p = sys::cstruct_new(76);
    sys::cstruct_set_string($p, "name", 0, $name);
    sys::cstruct_set_int($p, "age", 64, $age);
    sys::cstruct_set_double($p, "salary", 68, $salary);
    return $p;
}

func person_name(scalar $p) str {
    return sys::cstruct_get_string($p, "name", 0);
}

func person_age(scalar $p) int {
    return sys::cstruct_get_int($p, "age", 64);
}

func person_salary(scalar $p) num {
    return sys::cstruct_get_double($p, "salary", 68);
}
```

### cstruct API Reference

| Function | Description |
|----------|-------------|
| `sys::cstruct_new($size)` | Allocate $size bytes |
| `sys::cstruct_set_int($s, $name, $offset, $val)` | Set 4-byte int |
| `sys::cstruct_get_int($s, $name, $offset)` | Get 4-byte int |
| `sys::cstruct_set_double($s, $name, $offset, $val)` | Set 8-byte double |
| `sys::cstruct_get_double($s, $name, $offset)` | Get 8-byte double |
| `sys::cstruct_set_string($s, $name, $offset, $val)` | Set string |
| `sys::cstruct_get_string($s, $name, $offset)` | Get string |

## Structs vs Hashes vs OOP

When should you use each?

### Use Structs When:

- You need fixed, known fields
- Performance matters (direct memory access)
- Interfacing with C code
- Memory layout must be predictable

```strada
struct Point { int x; int y; }
my Point $p;
$p->x = 10;  # Direct memory access, fast
```

### Use Hashes When:

- Fields are dynamic or unknown at compile time
- Flexibility is more important than speed
- Rapid prototyping

```strada
my hash %point = { "x" => 10, "y" => 20 };
$point{"z"} = 30;  # Can add fields anytime
```

### Use OOP When:

- You need inheritance
- You need methods and polymorphism
- You need destructors (DESTROY)

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

## Common Patterns

### Constructor Function

```strada
struct Config {
    str host;
    int port;
    int timeout;
}

func new_config(str $host, int $port) Config {
    my Config $c;
    $c->host = $host;
    $c->port = $port;
    $c->timeout = 30;  # Default value
    return $c;
}
```

### Struct Methods

Use naming conventions to group related functions:

```strada
struct Point { int x; int y; }

func Point_new(int $x, int $y) Point {
    my Point $p;
    $p->x = $x;
    $p->y = $y;
    return $p;
}

func Point_print(Point $p) void {
    say("(" . $p->x . ", " . $p->y . ")");
}

func Point_distance(Point $a, Point $b) num {
    my int $dx = $b->x - $a->x;
    my int $dy = $b->y - $a->y;
    return math::sqrt($dx * $dx + $dy * $dy);
}
```

### Array of Structs

```strada
struct Point { int x; int y; }

func main() int {
    my array @points;

    push(@points, Point_new(0, 0));
    push(@points, Point_new(10, 20));
    push(@points, Point_new(30, 40));

    my int $i = 0;
    while ($i < scalar(@points)) {
        Point_print($points[$i]);
        $i++;
    }

    return 0;
}
```

### Nested Data (Struct + Hash)

```strada
struct Employee {
    str name;
    int id;
}

func main() int {
    my hash %department;

    my Employee $alice;
    $alice->name = "Alice";
    $alice->id = 101;

    my Employee $bob;
    $bob->name = "Bob";
    $bob->id = 102;

    $department{"manager"} = $alice;
    $department{"developer"} = $bob;

    my Employee $mgr = $department{"manager"};
    say("Manager: " . $mgr->name);

    return 0;
}
```

## Memory Considerations

### Struct Memory

Structs are allocated on the stack or as part of the runtime's memory.
They are automatically cleaned up when they go out of scope.

```strada
func example() void {
    my Point $p;      # Allocated
    $p->x = 10;
    # $p automatically freed when function returns
}
```

### cstruct Memory

Low-level cstructs are reference counted like other Strada values:

```strada
my scalar $pt = sys::cstruct_new(8);
# When $pt goes out of scope or is set to undef, memory is freed
```

## Tips

1. **Prefer native syntax** - It's cleaner and less error-prone than manual offsets

2. **Document your layouts** - When using cstruct, comment the memory layout:
   ```strada
   # Point: x(int,0), y(int,4) = 8 bytes total
   ```

3. **Use constructor functions** - Don't repeat initialization code

4. **Name functions consistently** - Use `TypeName_method` pattern for methods

5. **Consider alignment** - When interfacing with C, match C struct alignment

## Examples

See these files for more examples:

- `examples/native_struct_syntax.strada` - Native struct syntax
- `examples/struct_definition_example.strada` - cstruct API patterns
- `examples/struct_funcs.strada` - Structs in functions
- `examples/test_struct_types.strada` - Various struct types
