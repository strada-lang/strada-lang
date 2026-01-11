# Regular Expressions in Strada

Strada provides Perl-style regular expression support through the `=~` and `!~` operators, regex substitution with `s///`, and several built-in functions for advanced regex operations.

## Basic Pattern Matching

Use the `=~` operator to test if a string matches a pattern:

```strada
my str $text = "Hello, World!";

if ($text =~ /World/) {
    say("Found World!");
}

# Negated match with !~
if ($text !~ /Goodbye/) {
    say("No Goodbye found");
}
```

## Pattern Syntax

Strada uses POSIX Extended Regular Expressions (ERE) with some Perl extensions.

### Anchors

| Pattern | Meaning |
|---------|---------|
| `^` | Start of string |
| `$` | End of string |
| `\b` | Word boundary |

```strada
# ^ matches start of string
if ("hello world" =~ /^hello/) { say("Starts with hello"); }

# $ matches end of string
if ("hello world" =~ /world$/) { say("Ends with world"); }

# Both anchors for exact match
if ("hello" =~ /^hello$/) { say("Exact match"); }

# IMPORTANT: $ in regex means end-of-line, NOT variable interpolation
# To use both a variable and $ anchor:
my str $suffix = "world";
if ("hello world" =~ /$suffix$/) { say("Ends with $suffix"); }
```

### Character Classes

| Pattern | Meaning |
|---------|---------|
| `.` | Any character except newline |
| `\d` | Digit (0-9) |
| `\D` | Non-digit |
| `\w` | Word character (a-z, A-Z, 0-9, _) |
| `\W` | Non-word character |
| `\s` | Whitespace (space, tab, newline) |
| `\S` | Non-whitespace |
| `[abc]` | Character class (a, b, or c) |
| `[^abc]` | Negated class (not a, b, or c) |
| `[a-z]` | Character range |

```strada
if ($text =~ /\d+/) { say("Contains digits"); }
if ($text =~ /[aeiou]/) { say("Contains a vowel"); }
if ($text =~ /[A-Z][a-z]+/) { say("Contains capitalized word"); }
```

### Quantifiers

| Pattern | Meaning |
|---------|---------|
| `*` | Zero or more |
| `+` | One or more |
| `?` | Zero or one |
| `{n}` | Exactly n times |
| `{n,}` | n or more times |
| `{n,m}` | Between n and m times |

```strada
if ($text =~ /a+/) { say("One or more a's"); }
if ($text =~ /\d{3}-\d{4}/) { say("Phone number format"); }
```

### Alternation and Grouping

| Pattern | Meaning |
|---------|---------|
| `\|` | Alternation (or) |
| `(...)` | Grouping and capture |
| `(?:...)` | Non-capturing group |

```strada
if ($text =~ /cat|dog/) { say("Found cat or dog"); }
if ($text =~ /(hello|hi) world/) { say("Greeting found"); }
```

## Flags/Modifiers

Flags are specified after the closing delimiter:

| Flag | Meaning |
|------|---------|
| `i` | Case-insensitive matching |
| `m` | Multi-line mode (^ and $ match line boundaries) |
| `s` | Single-line mode (. matches newline) |
| `g` | Global (for substitution - replace all) |

```strada
# Case-insensitive
if ($text =~ /hello/i) { say("Found hello (any case)"); }

# Multi-line
my str $lines = "line1\nline2";
if ($lines =~ /^line2/m) { say("Found line2 at start of line"); }
```

## Capturing Groups

Use parentheses to capture parts of the match. After a successful match, capture groups are available via the `$1` through `$9` variables or the `captures()` function.

### Capture Variables (`$1`-`$9`)

After a successful regex match, `$1` through `$9` provide direct access to capture groups. This is the simplest and most common way to access captures:

```strada
my str $date = "2024-01-15";

if ($date =~ /(\d{4})-(\d{2})-(\d{2})/) {
    say("Year: " . $1);    # 2024
    say("Month: " . $2);   # 01
    say("Day: " . $3);     # 15
}
```

These variables are syntactic sugar for `captures()[N]`. They return `undef` if the group does not exist or if no match has occurred.

```strada
if ("hello world" =~ /(\w+)\s+(\w+)/) {
    say($1);  # "hello"
    say($2);  # "world"
}
```

### The `captures()` Function

For programmatic access or when you need the full match (`$0`), use the `captures()` function which returns an array:

```strada
my str $date = "2024-01-15";

if ($date =~ /(\d{4})-(\d{2})-(\d{2})/) {
    my array @parts = captures();
    say("Full match: " . $parts[0]);   # 2024-01-15
    say("Year: " . $parts[1]);         # 2024
    say("Month: " . $parts[2]);        # 01
    say("Day: " . $parts[3]);          # 15
}
```

### Destructuring Captures

You can destructure captures directly into variables:

```strada
if ($date =~ /(\d{4})-(\d{2})-(\d{2})/) {
    my (scalar $full, scalar $year, scalar $month, scalar $day) = captures();
    say("Year: " . $year);
    say("Month: " . $month);
}
```

### Inline Capture Access

You can access individual captures directly without storing the array:

```strada
if ($line =~ /^(\w+)\s*=\s*(.*)$/) {
    say("Key: " . $1);
    say("Value: " . $2);
}
```

Or using `captures()` for index-based access:

```strada
if ($line =~ /^(\w+)\s*=\s*(.*)$/) {
    my scalar $key = captures()[1];
    my scalar $value = captures()[2];
}
```

**Note:** Each call to `captures()` creates a new array. If you need multiple capture groups, prefer using `$1`-`$9` directly or call `captures()` once and store the result:

```strada
# Most efficient: use $1-$9 directly
my scalar $key = $1;
my scalar $value = $2;

# Also efficient: one captures() call
my array @caps = captures();
my scalar $key = $caps[1];
my scalar $value = $caps[2];

# Less efficient: multiple captures() calls
my scalar $key = captures()[1];    # creates array, extracts, discards
my scalar $value = captures()[2];  # creates another array
```

### Named Captures (Not Currently Supported)

Strada currently uses positional captures only. Named captures are not yet implemented.

## Substitution

Use `s///` for search and replace:

```strada
my str $text = "Hello World";

# Replace first occurrence
$text =~ s/World/Strada/;
say($text);  # Hello Strada

# Replace all occurrences with /g flag
my str $repeated = "cat cat cat";
$repeated =~ s/cat/dog/g;
say($repeated);  # dog dog dog
```

### Case Modification in Replacement

```strada
my str $text = "hello world";

# \U - uppercase, \L - lowercase, \E - end modification
$text =~ s/hello/\Uhello\E/;  # Not currently supported
# Use uc() function instead:
$text = uc(substr($text, 0, 5)) . substr($text, 5);
```

## Variable Interpolation in Patterns

Variables are interpolated in regex patterns:

```strada
my str $search = "world";
if ($text =~ /$search/) {
    say("Found: $search");
}

# Combine variable with $ anchor (end of line)
my str $suffix = "end";
if ($text =~ /$suffix$/) {
    say("Text ends with $suffix");
}
```

### Escaping Special Characters

To match literal special characters, escape them with backslash:

```strada
# Match literal dot
if ($filename =~ /\.txt$/) { say("Text file"); }

# Match literal dollar sign - use two backslashes in Strada string
if ($price =~ /\$\d+/) { say("Price found"); }

# Common metacharacters that need escaping: . * + ? [ ] { } ( ) | ^ $ \
```

## Built-in Functions

### match()

Simple pattern match (returns 1 or 0):

```strada
my int $found = match($text, "pattern");
```

### capture()

Get all capture groups as an array:

```strada
my array @groups = capture($text, "(\d+)-(\d+)");
```

### captures()

Get capture groups from the most recent `=~` match as an array:

```strada
if ($text =~ /(\d+)-(\d+)/) {
    my array @parts = captures();
    say($parts[1]);  # First group
    say($parts[2]);  # Second group
}
```

### Capture Variables (`$1`-`$9`)

After a successful `=~` match, `$1` through `$9` provide direct access to capture groups. This is syntactic sugar for `captures()[N]`:

```strada
if ($text =~ /(\d+)-(\d+)/) {
    say($1);  # First group
    say($2);  # Second group
}
```

Returns `undef` if the group does not exist or if no match has occurred.

### split()

Split string by pattern:

```strada
my array @words = split("\\s+", $text);  # Split on whitespace
my array @parts = split(",", $csv);       # Split on comma
```

**Note:** `split()` uses regex patterns. To split on literal special characters, escape them:

```strada
my array @parts = split("\\.", $ip_address);  # Split on literal dot
my array @items = split("\\|", $data);        # Split on literal pipe
```

### join()

Join array with separator (not regex, but commonly used with split):

```strada
my str $text = join(", ", @items);
```

### replace()

Replace first match of a pattern:

```strada
my str $result = replace($text, "old", "new");
my str $clean = replace($input, "^\\s+", "");  # Trim leading whitespace
```

### replace_all()

Replace all matches of a pattern (like `s///g`):

```strada
my str $result = replace_all($text, "\\s+", " ");  # Collapse whitespace
my str $digits = replace_all($input, "[^0-9]", "");  # Keep only digits
```

**Note:** `replace()` and `replace_all()` cannot match a literal `$` character because POSIX regex always treats `$` as the end-of-line anchor. Use `index()` + `substr()` for string-based replacement when `$` is in the target.

## Common Patterns

### Email Validation

```strada
func is_valid_email(str $email) int {
    return match($email, "^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}$");
}
```

### Phone Number

```strada
func is_phone(str $phone) int {
    return match($phone, "^\\d{3}[-.]?\\d{3}[-.]?\\d{4}$");
}
```

### URL Extraction

```strada
func extract_urls(str $text) array {
    my array @urls = ();
    # This is simplified - real URL matching is more complex
    while ($text =~ /(https?:\/\/[^\s]+)/) {
        my array @cap = captures();
        push(@urls, $cap[1]);
        $text =~ s/https?:\/\/[^\s]+//;
    }
    return @urls;
}
```

### Whitespace Cleanup

```strada
# Trim leading/trailing whitespace
$text =~ s/^\s+//;
$text =~ s/\s+$//;

# Collapse multiple spaces to single
$text =~ s/\s+/ /g;
```

## Transliteration (`tr///` / `y///`)

The `tr///` operator (and its alias `y///`) performs character-by-character transliteration on a string. Unlike `s///`, it does not use regular expressions -- it maps individual characters from one set to another.

### Basic Syntax

```strada
$str =~ tr/SEARCHLIST/REPLACEMENTLIST/FLAGS;
$str =~ y/SEARCHLIST/REPLACEMENTLIST/FLAGS;    # y/// is an alias for tr///
```

The operator replaces every character in `SEARCHLIST` with the corresponding character in `REPLACEMENTLIST`. It returns the number of characters matched (or replaced/deleted).

### Simple Character Replacement

```strada
my str $text = "Hello, World!";
$text =~ tr/o/0/;          # Replace 'o' with '0'
say($text);                 # Hell0, W0rld!

my str $msg = "secret";
$msg =~ tr/a-z/A-Z/;       # Uppercase all lowercase letters
say($msg);                  # SECRET
```

### Character Ranges

Ranges are specified with `-` between start and end characters:

```strada
# Lowercase to uppercase
$str =~ tr/a-z/A-Z/;

# Uppercase to lowercase
$str =~ tr/A-Z/a-z/;

# ROT13 cipher
$str =~ tr/a-zA-Z/n-za-mN-ZA-M/;

# Replace digits with asterisks
$str =~ tr/0-9/*/;
```

### Flags

| Flag | Meaning |
|------|---------|
| `c` | Complement -- transliterate characters NOT in SEARCHLIST |
| `d` | Delete -- delete characters found in SEARCHLIST that have no replacement |
| `s` | Squeeze -- collapse duplicate consecutive replaced characters into one |
| `r` | Return -- return a modified copy instead of modifying in place |

#### `d` (Delete) Flag

Delete characters in SEARCHLIST that have no corresponding character in REPLACEMENTLIST:

```strada
my str $text = "Hello 123 World 456";
$text =~ tr/0-9//d;         # Delete all digits
say($text);                  # Hello  World
```

#### `s` (Squeeze) Flag

After transliteration, collapse consecutive duplicate replacement characters into one:

```strada
my str $text = "too    many   spaces";
$text =~ tr/ / /s;          # Squeeze multiple spaces into one
say($text);                  # too many spaces

my str $letters = "aaabbbccc";
$letters =~ tr/a-c/x/s;     # Replace a,b,c with x, then squeeze
say($letters);               # x
```

#### `c` (Complement) Flag

Transliterate characters NOT in SEARCHLIST:

```strada
my str $text = "Hello, World! 123";
$text =~ tr/a-zA-Z//cd;     # Delete non-alpha characters
say($text);                  # HelloWorld

my str $data = "abc\x01\x02def";
$data =~ tr/\x20-\x7E//cd;  # Keep only printable ASCII
say($data);                  # abcdef
```

#### `r` (Return Copy) Flag

Return a modified copy instead of modifying the original:

```strada
my str $original = "Hello World";
my str $upper = ($original =~ tr/a-z/A-Z/r);
say($original);              # Hello World  (unchanged)
say($upper);                 # HELLO WORLD
```

### Return Value (Count)

`tr///` returns the number of characters that were transliterated (or matched):

```strada
my str $text = "Hello World";
my int $vowels = ($text =~ tr/aeiouAEIOU//);
say("Vowel count: " . $vowels);   # Vowel count: 3

# Count without modifying (empty replacement = count only)
my str $data = "abc123def456";
my int $digits = ($data =~ tr/0-9//);
say("Digit count: " . $digits);   # Digit count: 6
```

### Combining Flags

Flags can be combined:

```strada
# Delete non-alphanumeric and squeeze spaces
my str $messy = "  Hello,   World!!  123  ";
$messy =~ tr/a-zA-Z0-9 //cd;   # Delete non-alnum/space (complement + delete)
$messy =~ tr/ / /s;             # Squeeze spaces
say($messy);                     # Hello World 123
```

---

## The `/e` Modifier (Evaluate Replacement)

The `/e` modifier on `s///` evaluates the replacement string as a Strada expression. This allows dynamic replacements where the replacement is computed from the match.

### Syntax

```strada
$str =~ s/PATTERN/EXPRESSION/e;
$str =~ s/PATTERN/EXPRESSION/eg;   # With global flag
```

The replacement side is evaluated as Strada code. The result of the expression becomes the replacement string.

### Basic Usage

```strada
# Double every number in the string
my str $text = "I have 3 cats and 7 dogs";
$text =~ s/(\d+)/captures()[1] * 2/eg;
say($text);   # I have 6 cats and 14 dogs

# Uppercase matched words
my str $msg = "hello world";
$msg =~ s/(\w+)/uc(captures()[1])/eg;
say($msg);   # HELLO WORLD
```

### Using Capture Variables in `/e` Replacement

Inside a `/e` replacement, `$1` through `$9` (and `captures()`) give access to the capture groups from the pattern:

```strada
my str $template = "Name: {name}, Age: {age}";
my hash %data = ();
$data{"name"} = "Alice";
$data{"age"} = "30";

$template =~ s/\{(\w+)\}/$data{$1}/eg;
say($template);   # Name: Alice, Age: 30
```

### Works with `/g` (Global)

The `/e` modifier is commonly combined with `/g` to replace all occurrences:

```strada
# Increment all numbers
my str $version = "1.2.3";
$version =~ s/(\d+)/captures()[1] + 1/eg;
say($version);   # 2.3.4

# Wrap each word in brackets
my str $words = "foo bar baz";
$words =~ s/(\w+)/"[" . captures()[1] . "]"/eg;
say($words);   # [foo] [bar] [baz]
```

### Expression Complexity

The replacement can be any valid Strada expression, including function calls:

```strada
func encode_char(str $ch) str {
    return "%" . core::sprintf("%02X", ord($ch));
}

my str $url = "hello world!";
$url =~ s/([^a-zA-Z0-9])/encode_char(captures()[1])/eg;
say($url);   # hello%20world%21
```

---

## Important Notes

### $ in Regex vs Variable Interpolation

The `$` character has two meanings:
1. **End-of-line anchor** when at end of pattern or not followed by a word character
2. **Variable interpolation** when followed by a variable name

```strada
# $ as anchor (end of line)
if ($text =~ /end$/) { ... }

# $ as variable interpolation
my str $var = "test";
if ($text =~ /$var/) { ... }

# Both together - variable followed by anchor
if ($text =~ /$var$/) { ... }  # Match $var at end of string
```

### POSIX vs Perl Regex

Strada uses POSIX Extended Regular Expressions. Some Perl features are not available:
- Lookahead/lookbehind (`(?=...)`, `(?!...)`, etc.)
- Named captures (`(?<name>...)`)
- Possessive quantifiers (`*+`, `++`)
- Unicode properties (`\p{...}`)

### Performance Considerations

- Regex patterns are compiled each time they're used
- For repeated matching with the same pattern, consider storing the pattern in a variable
- Complex patterns with many alternations can be slow
- Use anchors (`^`, `$`) when possible to limit search space

## Troubleshooting

### Pattern Not Matching

1. Check anchor usage - `^` and `$` match start/end of entire string by default
2. Verify escaping - special characters need backslash
3. Use `/i` flag for case-insensitive matching
4. Test pattern components separately

### Captures Not Working

1. Ensure pattern actually matched before calling `captures()` or using `$1`-`$9`
2. Remember `captures()[0]` is the full match, groups start at index 1 (and `$1`)
3. Captures are cleared on each match - save them if needed
4. `$1`-`$9` return `undef` if the group does not exist

```strada
if ($text =~ /pattern/) {
    # Use $1-$9 directly (simplest)
    my str $first = $1;

    # Or save captures() array for later use
    my array @saved = captures();  # Save immediately
    # ... use @saved later
}
```

### Special Characters in Replacement

In substitution replacements, most characters are literal. Use function calls for case changes:

```strada
# Instead of \U for uppercase:
my str $word = "hello";
$word =~ s/hello/HELLO/;  # Literal replacement
# Or:
$word = uc($word);        # Using uc() function
```
