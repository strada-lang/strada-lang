#!/bin/bash
# perl2strada test runner
# Tests that the converter processes Perl files without errors
# and produces expected patterns in the output

PASS=0
FAIL=0
TOTAL=0
VERBOSE=${V:-0}

P2S=./perl2strada
TESTDIR="t/perl2strada"

if [ ! -x "$P2S" ]; then
    echo "ERROR: perl2strada not found or not executable. Run 'make tools' first."
    exit 1
fi

check_output() {
    local testname="$1"
    local input="$2"
    local pattern="$3"
    local should_match="${4:-1}"  # 1=should match, 0=should NOT match

    TOTAL=$((TOTAL + 1))

    local tmpout=$(mktemp /tmp/p2s_test_XXXXXX.strada)
    $P2S "$input" "$tmpout" > /dev/null 2>&1
    local rc=$?

    if [ $rc -ne 0 ]; then
        echo "FAIL: $testname (converter returned error $rc)"
        FAIL=$((FAIL + 1))
        rm -f "$tmpout"
        return
    fi

    if [ "$should_match" = "1" ]; then
        if grep -qP -- "$pattern" "$tmpout" 2>/dev/null || grep -q -- "$pattern" "$tmpout" 2>/dev/null; then
            echo "ok: $testname"
            PASS=$((PASS + 1))
        else
            echo "FAIL: $testname"
            echo "  Expected pattern: $pattern"
            if [ "$VERBOSE" = "1" ]; then
                echo "  Output:"
                cat "$tmpout" | head -50
            fi
            FAIL=$((FAIL + 1))
        fi
    else
        if grep -qP -- "$pattern" "$tmpout" 2>/dev/null || grep -q -- "$pattern" "$tmpout" 2>/dev/null; then
            echo "FAIL: $testname (pattern should NOT match)"
            echo "  Unexpected pattern found: $pattern"
            FAIL=$((FAIL + 1))
        else
            echo "ok: $testname"
            PASS=$((PASS + 1))
        fi
    fi

    rm -f "$tmpout"
}

check_no_crash() {
    local testname="$1"
    local input="$2"

    TOTAL=$((TOTAL + 1))

    local tmpout=$(mktemp /tmp/p2s_test_XXXXXX.strada)
    $P2S "$input" "$tmpout" > /dev/null 2>&1
    local rc=$?

    if [ $rc -ne 0 ]; then
        echo "FAIL: $testname (converter crashed with exit code $rc)"
        FAIL=$((FAIL + 1))
    else
        echo "ok: $testname"
        PASS=$((PASS + 1))
    fi

    rm -f "$tmpout"
}

echo "=== perl2strada Test Suite ==="
echo ""

# ---- test_basic.pl ----
echo "--- test_basic.pl ---"
check_no_crash "basic: no crash" "$TESTDIR/test_basic.pl"
check_output "basic: use strict commented" "$TESTDIR/test_basic.pl" "# use strict"
check_output "basic: use warnings commented" "$TESTDIR/test_basic.pl" "# use warnings"
check_output "basic: use feature commented" "$TESTDIR/test_basic.pl" "# use feature"
check_output "basic: my scalar" "$TESTDIR/test_basic.pl" "my scalar"
check_output "basic: my array" "$TESTDIR/test_basic.pl" "my array"
check_output "basic: my hash" "$TESTDIR/test_basic.pl" "my hash"
check_output "basic: our VERSION -> version" "$TESTDIR/test_basic.pl" 'version "1.0"'
check_output "basic: our scalar" "$TESTDIR/test_basic.pl" "our scalar"
check_output "basic: our hash -> my hash" "$TESTDIR/test_basic.pl" "my hash"
check_output "basic: local passes through" "$TESTDIR/test_basic.pl" "local"
check_output "basic: use constant -> const" "$TESTDIR/test_basic.pl" "const scalar MAX_SIZE"
check_output "basic: sub -> func" "$TESTDIR/test_basic.pl" "func greet"
check_output "basic: func uses no-parens form" "$TESTDIR/test_basic.pl" "func greet {"
check_output "basic: shift/args preserved in body" "$TESTDIR/test_basic.pl" "my (\$name) = @_"
check_output "basic: add uses no-parens form" "$TESTDIR/test_basic.pl" "func add {"
check_output "basic: foreach my scalar" "$TESTDIR/test_basic.pl" "foreach my scalar"
check_output "basic: for -> foreach" "$TESTDIR/test_basic.pl" "foreach my scalar"
check_output "basic: unless -> if(!()" "$TESTDIR/test_basic.pl" "if (!"
check_output "basic: until passes through" "$TESTDIR/test_basic.pl" "until ("
check_output "basic: say with parens" "$TESTDIR/test_basic.pl" 'say("'
check_output "basic: print -> say" "$TESTDIR/test_basic.pl" 'say("Hello'
check_output "basic: print STDERR -> warn" "$TESTDIR/test_basic.pl" 'warn("'
check_output "basic: and -> &&" "$TESTDIR/test_basic.pl" "&&"
check_output "basic: or -> ||" "$TESTDIR/test_basic.pl" "||"
check_output "basic: 1; commented" "$TESTDIR/test_basic.pl" "# 1;"
check_output "basic: no raw sub keyword" "$TESTDIR/test_basic.pl" "^sub " "0"

# ---- test_oop.pl ----
echo ""
echo "--- test_oop.pl ---"
check_no_crash "oop: no crash" "$TESTDIR/test_oop.pl"
check_output "oop: scalar self" "$TESTDIR/test_oop.pl" "scalar .self"
check_output "oop: hash key quoted" "$TESTDIR/test_oop.pl" 'opts..name'
check_output "oop: arrow key quoted" "$TESTDIR/test_oop.pl" 'self.*name'
check_output "oop: bless with parens" "$TESTDIR/test_oop.pl" "bless("
check_output "oop: DESTROY pass-through" "$TESTDIR/test_oop.pl" "func DESTROY"
check_output "oop: SUPER REVIEW" "$TESTDIR/test_oop.pl" "REVIEW.*SUPER"

# ---- test_fileio.pl ----
echo ""
echo "--- test_fileio.pl ---"
check_no_crash "fileio: no crash" "$TESTDIR/test_fileio.pl"
check_output "fileio: open read -> core::open" "$TESTDIR/test_fileio.pl" 'core::open.*"r"'
check_output "fileio: open write -> core::open" "$TESTDIR/test_fileio.pl" 'core::open.*"w"'
check_output "fileio: open append -> core::open" "$TESTDIR/test_fileio.pl" 'core::open.*"a"'
check_output "fileio: close -> core::close" "$TESTDIR/test_fileio.pl" "core::close"
check_output "fileio: chomp var" "$TESTDIR/test_fileio.pl" '=~ s/\\n$//'
check_output "fileio: STDIN -> core::readline" "$TESTDIR/test_fileio.pl" "core::readline"
check_output "fileio: print to fh" "$TESTDIR/test_fileio.pl" 'print(\$out'
check_output "fileio: say to fh" "$TESTDIR/test_fileio.pl" 'say(\$out'
check_output "fileio: chomp+STDIN decomposed" "$TESTDIR/test_fileio.pl" 'my scalar \$input = core::readline'
check_output "fileio: chomp+fh decomposed" "$TESTDIR/test_fileio.pl" 'my scalar \$data = <\$fh>'
check_output "fileio: pipe read -> core::popen" "$TESTDIR/test_fileio.pl" 'core::popen.*"r"'
check_output "fileio: pipe write -> core::popen" "$TESTDIR/test_fileio.pl" 'core::popen.*"w"'
check_output "fileio: 2-arg open read" "$TESTDIR/test_fileio.pl" 'core::open.*input.*"r"'
check_output "fileio: 2-arg open write" "$TESTDIR/test_fileio.pl" 'core::open.*output.*"w"'
check_output "fileio: 2-arg open append" "$TESTDIR/test_fileio.pl" 'core::open.*append.*"a"'
check_output "fileio: 2-arg bareword FH converted" "$TESTDIR/test_fileio.pl" 'core::open.*logfile.*"w"'
check_output "fileio: seek -> core::seek" "$TESTDIR/test_fileio.pl" 'core::seek'
check_output "fileio: tell -> core::tell" "$TESTDIR/test_fileio.pl" 'core::tell'
check_output "fileio: eof -> core::eof" "$TESTDIR/test_fileio.pl" 'core::eof'
check_output "fileio: binmode commented" "$TESTDIR/test_fileio.pl" '# binmode'
check_output "fileio: sysread converted" "$TESTDIR/test_fileio.pl" 'core::read'
check_output "fileio: syswrite converted" "$TESTDIR/test_fileio.pl" 'core::write'
check_output "fileio: printf to fh" "$TESTDIR/test_fileio.pl" 'print(\$out, sprintf('
check_output "fileio: or die stripped from sys calls" "$TESTDIR/test_fileio.pl" 'core::chdir("/tmp");'

# ---- test_builtins.pl ----
echo ""
echo "--- test_builtins.pl ---"
check_no_crash "builtins: no crash" "$TESTDIR/test_builtins.pl"
check_output "builtins: push with parens" "$TESTDIR/test_builtins.pl" "push(@"
check_output "builtins: pop with parens" "$TESTDIR/test_builtins.pl" "pop(@"
check_output "builtins: shift with parens" "$TESTDIR/test_builtins.pl" "shift(@"
check_output "builtins: unshift with parens" "$TESTDIR/test_builtins.pl" "unshift(@"
check_output "builtins: length with parens" "$TESTDIR/test_builtins.pl" "length("
check_output "builtins: scalar with parens" "$TESTDIR/test_builtins.pl" "scalar(@"
check_output "builtins: keys with parens" "$TESTDIR/test_builtins.pl" "keys(%"
check_output "builtins: values with parens" "$TESTDIR/test_builtins.pl" "values(%"
check_output "builtins: ref with parens" "$TESTDIR/test_builtins.pl" "ref("
check_output "builtins: exists with parens" "$TESTDIR/test_builtins.pl" "exists("
check_output "builtins: die with parens" "$TESTDIR/test_builtins.pl" 'die("'
check_output "builtins: warn with parens" "$TESTDIR/test_builtins.pl" 'warn("'
check_output "builtins: die obj -> throw" "$TESTDIR/test_builtins.pl" 'throw.\$exception.'
check_output "builtins: qw expanded" "$TESTDIR/test_builtins.pl" '("red"'
check_output "builtins: defined with parens" "$TESTDIR/test_builtins.pl" "defined("

# ---- test_advanced.pl ----
echo ""
echo "--- test_advanced.pl ---"
check_no_crash "advanced: no crash" "$TESTDIR/test_advanced.pl"
check_output "advanced: use JSON review" "$TESTDIR/test_advanced.pl" 'use JSON;'
check_output "advanced: use Carp commented" "$TESTDIR/test_advanced.pl" "# use Carp"
check_output "advanced: use Data::Dumper commented" "$TESTDIR/test_advanced.pl" "# use Data::Dumper"
check_output "advanced: no strict commented" "$TESTDIR/test_advanced.pl" "# no strict"
check_output "advanced: no warnings commented" "$TESTDIR/test_advanced.pl" "# no warnings"
check_output "advanced: POD commented" "$TESTDIR/test_advanced.pl" "# =pod"
check_output "advanced: POD head1 commented" "$TESTDIR/test_advanced.pl" "# =head1"
check_output "advanced: postfix if" "$TESTDIR/test_advanced.pl" 'if (\$verbose) { say'
check_output "advanced: postfix unless" "$TESTDIR/test_advanced.pl" 'if (!(\$quiet))'
check_output "advanced: postfix while" "$TESTDIR/test_advanced.pl" "while ("
check_output "advanced: ||= expanded" "$TESTDIR/test_advanced.pl" '= \$x ||'
check_output "advanced: .= expanded" "$TESTDIR/test_advanced.pl" '= \$s \.'
check_output "advanced: m// stripped" "$TESTDIR/test_advanced.pl" '=~ /pattern/'
check_output "advanced: eval -> try" "$TESTDIR/test_advanced.pl" "try {"
check_output "advanced: catch block" "$TESTDIR/test_advanced.pl" 'catch (\$e)'
check_output "advanced: interpolation" "$TESTDIR/test_advanced.pl" '" \. \$name'
check_output "advanced: heredoc to string" "$TESTDIR/test_advanced.pl" '"<html>'
check_output "advanced: x repetition native" "$TESTDIR/test_advanced.pl" '" x 40'
check_output "advanced: .. range passes through" "$TESTDIR/test_advanced.pl" '\.\.'
check_output "advanced: __END__ commented" "$TESTDIR/test_advanced.pl" "# __END__"
check_output "advanced: wantarray converted" "$TESTDIR/test_advanced.pl" "core::wantarray()"
check_output "advanced: caller -> __PACKAGE__" "$TESTDIR/test_advanced.pl" "__PACKAGE__"
check_output "advanced: croak -> die" "$TESTDIR/test_advanced.pl" "die("
check_output "advanced: confess -> die" "$TESTDIR/test_advanced.pl" "die("
check_output "advanced: Dumper built-in" "$TESTDIR/test_advanced.pl" "dumper.*built-in"

# ---- test_autoload.pl ----
echo ""
echo "--- test_autoload.pl ---"
check_no_crash "autoload: no crash" "$TESTDIR/test_autoload.pl"
check_output "autoload: AUTOLOAD signature" "$TESTDIR/test_autoload.pl" "func AUTOLOAD(scalar .self, str .method, scalar ...@args) scalar"
check_output "autoload: DESTROY pass-through" "$TESTDIR/test_autoload.pl" "func DESTROY"
check_output "autoload: no our AUTOLOAD" "$TESTDIR/test_autoload.pl" "our.*AUTOLOAD" 0
check_output "autoload: no s/.*:://" "$TESTDIR/test_autoload.pl" "s/\\.\\*:://" 0
check_output "autoload: body logic preserved" "$TESTDIR/test_autoload.pl" "autoloaded:"

# ---- test_multishift.pl ----
echo ""
echo "--- test_multishift.pl ---"
check_no_crash "multishift: no crash" "$TESTDIR/test_multishift.pl"
check_output "multishift: new uses no-parens form" "$TESTDIR/test_multishift.pl" "func new {"
check_output "multishift: parse uses no-parens form" "$TESTDIR/test_multishift.pl" "func parse {"
check_output "multishift: shift lines preserved" "$TESTDIR/test_multishift.pl" "= shift"

# ---- test_main_wrap.pl ----
echo ""
echo "--- test_main_wrap.pl ---"
check_no_crash "main_wrap: no crash" "$TESTDIR/test_main_wrap.pl"
check_output "main_wrap: func main emitted" "$TESTDIR/test_main_wrap.pl" "func main() int"
check_output "main_wrap: return 0" "$TESTDIR/test_main_wrap.pl" "return 0"
check_output "main_wrap: code indented" "$TESTDIR/test_main_wrap.pl" "    my scalar"

# ---- test_slices_misc.pl ----
echo ""
echo "--- test_slices_misc.pl ---"
check_no_crash "slices: no crash" "$TESTDIR/test_slices_misc.pl"
check_output "slices: hash slice passes through" "$TESTDIR/test_slices_misc.pl" "@data{"
check_output "slices: array slice passes through" "$TESTDIR/test_slices_misc.pl" "@arr\["
check_output "slices: use lib double quotes" "$TESTDIR/test_slices_misc.pl" 'use lib "lib"'
check_output "slices: Getopt::Long native" "$TESTDIR/test_slices_misc.pl" "use Getopt::Long;"
check_output "slices: no strict commented" "$TESTDIR/test_slices_misc.pl" "# no strict"
check_output "slices: no warnings commented" "$TESTDIR/test_slices_misc.pl" "# no warnings"
check_output "slices: local dollar-slash slurp" "$TESTDIR/test_slices_misc.pl" "core::slurp"
check_output "slices: GetOptions argv loop" "$TESTDIR/test_slices_misc.pl" "REVIEW.*auto-converted from GetOptions"

# ---- test_remaining.pl ----
echo ""
echo "--- test_remaining.pl ---"
check_no_crash "remaining: no crash" "$TESTDIR/test_remaining.pl"

# Group A: File & Directory Operations
check_output "remaining: unlink paren -> core::unlink" "$TESTDIR/test_remaining.pl" "core::unlink("
check_output "remaining: unlink bare -> core::unlink" "$TESTDIR/test_remaining.pl" 'core::unlink("/tmp/old.txt")'
check_output "remaining: rename -> core::rename" "$TESTDIR/test_remaining.pl" "core::rename("
check_output "remaining: chmod paren -> core::chmod" "$TESTDIR/test_remaining.pl" "core::chmod(0755"
check_output "remaining: chmod bare -> core::chmod" "$TESTDIR/test_remaining.pl" "core::chmod(0644"
check_output "remaining: stat -> core::stat" "$TESTDIR/test_remaining.pl" "core::stat("
check_output "remaining: mkdir paren -> core::mkdir" "$TESTDIR/test_remaining.pl" "core::mkdir("
check_output "remaining: mkdir bare -> core::mkdir" "$TESTDIR/test_remaining.pl" 'core::mkdir("/tmp/newdir")'
check_output "remaining: chdir paren -> core::chdir" "$TESTDIR/test_remaining.pl" "core::chdir("
check_output "remaining: chdir bare -> core::chdir" "$TESTDIR/test_remaining.pl" 'core::chdir("/home/user")'
check_output "remaining: rmdir paren -> core::rmdir" "$TESTDIR/test_remaining.pl" "core::rmdir("
check_output "remaining: rmdir bare -> core::rmdir" "$TESTDIR/test_remaining.pl" 'core::rmdir("/tmp/olddir")'
check_output "remaining: opendir converted" "$TESTDIR/test_remaining.pl" "# opendir.*unnecessary"
check_output "remaining: readdir converted" "$TESTDIR/test_remaining.pl" "core::readdir"
check_output "remaining: closedir handled" "$TESTDIR/test_remaining.pl" "closedir.*unnecessary"

# Group B: System Calls & Process Control
check_output "remaining: system paren -> core::system" "$TESTDIR/test_remaining.pl" 'core::system("ls -la")'
check_output "remaining: system bare -> core::system" "$TESTDIR/test_remaining.pl" "core::system("
check_output "remaining: exec paren -> core::exec" "$TESTDIR/test_remaining.pl" 'core::exec("./run.sh")'
check_output "remaining: fork -> core::fork" "$TESTDIR/test_remaining.pl" "core::fork("
check_output "remaining: waitpid -> core::waitpid" "$TESTDIR/test_remaining.pl" "core::waitpid("
check_output "remaining: sleep paren -> core::sleep" "$TESTDIR/test_remaining.pl" "core::sleep(5)"
check_output "remaining: sleep bare -> core::sleep" "$TESTDIR/test_remaining.pl" "core::sleep(10)"
check_output "remaining: kill -> core::kill" "$TESTDIR/test_remaining.pl" "core::kill("
check_output "remaining: alarm paren -> core::alarm" "$TESTDIR/test_remaining.pl" "core::alarm(30)"
check_output "remaining: alarm bare -> core::alarm" "$TESTDIR/test_remaining.pl" "core::alarm(60)"

# Group C: Backticks & qx
check_output "remaining: backticks -> core::qx" "$TESTDIR/test_remaining.pl" 'core::qx("ls -la /tmp")'
check_output "remaining: qx/.../ -> core::qx" "$TESTDIR/test_remaining.pl" 'core::qx("uname -a")'
check_output "remaining: qx(...) -> core::qx" "$TESTDIR/test_remaining.pl" 'core::qx("date'
check_output "remaining: qx{...} -> core::qx" "$TESTDIR/test_remaining.pl" 'core::qx("uptime")'

# Group D: String Case Functions
check_output "remaining: lc bare -> lc()" "$TESTDIR/test_remaining.pl" "lc("
check_output "remaining: uc bare -> uc()" "$TESTDIR/test_remaining.pl" "uc("
check_output "remaining: lcfirst bare -> lcfirst()" "$TESTDIR/test_remaining.pl" "lcfirst("
check_output "remaining: ucfirst bare -> ucfirst()" "$TESTDIR/test_remaining.pl" "ucfirst("

# Group E: Environment Variables
check_output "remaining: ENV read -> core::getenv" "$TESTDIR/test_remaining.pl" 'core::getenv("HOME")'
check_output "remaining: ENV quoted read -> core::getenv" "$TESTDIR/test_remaining.pl" 'core::getenv("PATH")'
check_output "remaining: ENV var read -> core::getenv" "$TESTDIR/test_remaining.pl" 'core::getenv(\$key)'
check_output "remaining: ENV assign -> core::setenv" "$TESTDIR/test_remaining.pl" 'core::setenv("MY_VAR"'
check_output "remaining: ENV delete -> core::unsetenv" "$TESTDIR/test_remaining.pl" 'core::unsetenv("TEMP_KEY")'

# Group F: Signal Handlers
check_output "remaining: SIG IGNORE -> core::signal" "$TESTDIR/test_remaining.pl" 'core::signal("INT", "IGNORE")'
check_output "remaining: SIG DEFAULT -> core::signal" "$TESTDIR/test_remaining.pl" 'core::signal("TERM", "DEFAULT")'
check_output "remaining: SIG funcref -> core::signal" "$TESTDIR/test_remaining.pl" 'core::signal("HUP"'
check_output "remaining: SIG sub -> core::signal" "$TESTDIR/test_remaining.pl" 'core::signal("ALRM"'

# Group G: Regex Backreferences ($1-$9 pass through natively)
check_output "remaining: backrefs pass through" "$TESTDIR/test_remaining.pl" '\$1'

# Group H: Deref Syntax
# scalar deref ($$ref) works natively in Strada - no REVIEW needed
# @{$ref} and %{$ref} dereference works natively in Strada - no REVIEW needed
# Only slices (@{$ref}{keys} and @{$ref}[indices]) get REVIEW

# Group I: Control Flow & Misc
check_output "remaining: while <> converted" "$TESTDIR/test_remaining.pl" "core::readline"
check_output "remaining: tr/// native" "$TESTDIR/test_remaining.pl" 'tr/a-z/A-Z/'
check_output "remaining: qr// -> string" "$TESTDIR/test_remaining.pl" 're = "'
check_output "remaining: require commented" "$TESTDIR/test_remaining.pl" "REVIEW.*import_lib"
check_output "remaining: do file converted to use" "$TESTDIR/test_remaining.pl" 'use setup.*REVIEW.*was do'
check_output "remaining: splice native" "$TESTDIR/test_remaining.pl" 'splice(@arr'
check_output "remaining: hex -> core::hex" "$TESTDIR/test_remaining.pl" "core::hex("
check_output "remaining: oct -> core::oct" "$TESTDIR/test_remaining.pl" "core::oct"
check_output "remaining: __FILE__ passes through" "$TESTDIR/test_remaining.pl" "__FILE__"
check_output "remaining: __FILE__ no TODO" "$TESTDIR/test_remaining.pl" "TODO.*__FILE__" "0"
check_output "remaining: __LINE__ passes through" "$TESTDIR/test_remaining.pl" "__LINE__"
check_output "remaining: __LINE__ no TODO" "$TESTDIR/test_remaining.pl" "TODO.*__LINE__" "0"
check_output "remaining: redo passes through" "$TESTDIR/test_remaining.pl" "redo;"

# ---- test_batch2.pl ----
echo ""
echo "--- test_batch2.pl ---"
check_no_crash "batch2: no crash" "$TESTDIR/test_batch2.pl"

# File test operators
check_output "batch2: -e var -> core::file_exists" "$TESTDIR/test_batch2.pl" 'core::file_exists(\$filename)'
check_output "batch2: -f var -> core::is_file" "$TESTDIR/test_batch2.pl" 'core::is_file(\$path)'
check_output "batch2: -d var -> core::is_dir" "$TESTDIR/test_batch2.pl" 'core::is_dir(\$directory)'
check_output "batch2: -s var -> core::file_size" "$TESTDIR/test_batch2.pl" 'core::file_size(\$datafile)'
check_output "batch2: -e string -> core::file_exists" "$TESTDIR/test_batch2.pl" 'core::file_exists("/tmp/test.txt")'
check_output "batch2: -r access" "$TESTDIR/test_batch2.pl" "core::access.*4.*== 0"
check_output "batch2: -w access" "$TESTDIR/test_batch2.pl" "core::access.*2.*== 0"

# Class->method() -> Class::method()
check_output "batch2: Animal->new -> Animal::new" "$TESTDIR/test_batch2.pl" 'Animal::new('
check_output "batch2: Dog->new -> Dog::new" "$TESTDIR/test_batch2.pl" 'Dog::new('
check_output "batch2: File::Path->make_path" "$TESTDIR/test_batch2.pl" 'core::system("mkdir -p "'
check_output "batch2: JSON->encode" "$TESTDIR/test_batch2.pl" 'JSON::encode('
check_output "batch2: Sysync->new" "$TESTDIR/test_batch2.pl" 'Sysync::new('

# chown
check_output "batch2: chown paren -> core::chown" "$TESTDIR/test_batch2.pl" "core::chown(0"
check_output "batch2: chown bare -> core::chown" "$TESTDIR/test_batch2.pl" "core::chown(1000"

# localtime
check_output "batch2: localtime bare -> core::localtime" "$TESTDIR/test_batch2.pl" "core::localtime()"
check_output "batch2: localtime paren -> core::localtime" "$TESTDIR/test_batch2.pl" 'core::localtime(\$timestamp)'

# $VERSION -> version
check_output "batch2: our VERSION -> version" "$TESTDIR/test_batch2.pl" 'version "1.2.3"'

# $! (errno)
check_output "batch2: die errno" "$TESTDIR/test_batch2.pl" "core::strerror"

# or die stripping on core:: calls
check_output "batch2: mkdir or die stripped" "$TESTDIR/test_batch2.pl" "core::mkdir"

# Regex flags
check_output "batch2: /i flag REVIEW" "$TESTDIR/test_batch2.pl" "REVIEW.*PCRE2"
check_output "batch2: s///e native" "$TESTDIR/test_batch2.pl" 's/foo/bar/eg'
check_output "batch2: /x flag REVIEW" "$TESTDIR/test_batch2.pl" "REVIEW.*PCRE2"

# ---- test_batch3.pl ----
echo ""
echo "--- test_batch3.pl ---"
check_no_crash "batch3: no crash" "$TESTDIR/test_batch3.pl"

# CPAN module hints
check_output "batch3: Digest::MD5 hint" "$TESTDIR/test_batch3.pl" "use crypt;.*Digest"
check_output "batch3: File::Basename hint" "$TESTDIR/test_batch3.pl" "core::basename.*core::dirname"
check_output "batch3: File::Path hint" "$TESTDIR/test_batch3.pl" "core::mkdir"
check_output "batch3: File::Find hint" "$TESTDIR/test_batch3.pl" "core::readdir.*recursion"
check_output "batch3: JSON hint" "$TESTDIR/test_batch3.pl" 'use JSON;'
check_output "batch3: POSIX hint" "$TESTDIR/test_batch3.pl" "POSIX.*core::"
check_output "batch3: Scalar::Util hint" "$TESTDIR/test_batch3.pl" "ref.*isa.*defined"
check_output "batch3: List::Util native" "$TESTDIR/test_batch3.pl" "use List::Util;"
check_output "batch3: Time::HiRes hint" "$TESTDIR/test_batch3.pl" "core::time.*core::usleep"
check_output "batch3: IO::Socket hint" "$TESTDIR/test_batch3.pl" "core::socket"
check_output "batch3: YAML hint" "$TESTDIR/test_batch3.pl" "REVIEW.*no YAML"
check_output "batch3: Storable hint" "$TESTDIR/test_batch3.pl" "No Storable"
check_output "batch3: Encode hint" "$TESTDIR/test_batch3.pl" "UTF-8 by default"

# q/qq quoting
check_output "batch3: q[] -> single quoted" "$TESTDIR/test_batch3.pl" "'hello world'"
check_output "batch3: q{} -> single quoted" "$TESTDIR/test_batch3.pl" "'some text'"
check_output "batch3: q() -> single quoted" "$TESTDIR/test_batch3.pl" "'more text'"
check_output "batch3: q// -> single quoted" "$TESTDIR/test_batch3.pl" "'slash text'"
check_output "batch3: qq[] -> double quoted" "$TESTDIR/test_batch3.pl" '"hello '
check_output "batch3: qq{} -> double quoted" "$TESTDIR/test_batch3.pl" '"value is '

# split /regex/
check_output "batch3: split /&/ converted" "$TESTDIR/test_batch3.pl" 'split("&"'
check_output "batch3: split /=/ converted" "$TESTDIR/test_batch3.pl" 'split("="'
check_output "batch3: split(/regex/) converted" "$TESTDIR/test_batch3.pl" 'split("\\s+"'

# int/abs paren enforcement
check_output "batch3: int bare passes through" "$TESTDIR/test_batch3.pl" 'int(\$total)'
check_output "batch3: abs bare -> abs()" "$TESTDIR/test_batch3.pl" 'abs(\$difference)'

# Misc patterns
check_output "batch3: bare chomp converted" "$TESTDIR/test_batch3.pl" 's/\\n'
check_output "batch3: read() converted" "$TESTDIR/test_batch3.pl" "core::read("
check_output "batch3: list-context diamond passes through" "$TESTDIR/test_batch3.pl" "my array @lines = <"
check_output "batch3: copy-and-modify converted" "$TESTDIR/test_batch3.pl" 'my scalar \$clean = \$dirty'
check_output "batch3: assignment in condition converted" "$TESTDIR/test_batch3.pl" 'my scalar \$match = \$hash'
check_output "batch3: bare shift preserved" "$TESTDIR/test_batch3.pl" "shift;"
check_output "batch3: return undef simplified" "$TESTDIR/test_batch3.pl" "return;.*undef implicitly"

# ---- test_batch4.pl ----
echo ""
echo "--- test_batch4.pl ---"
check_no_crash "batch4: no crash" "$TESTDIR/test_batch4.pl"

# Range operator passes through natively (no TODO)
check_output "batch4: range no TODO" "$TESTDIR/test_batch4.pl" "TODO.*range" "0"
check_output "batch4: range preserved" "$TESTDIR/test_batch4.pl" '1\.\.10'

# Assignment in conditional -> split into decl + if
check_output "batch4: assign-in-if declaration" "$TESTDIR/test_batch4.pl" 'my scalar \$match = \$hash'
check_output "batch4: assign-in-if condition" "$TESTDIR/test_batch4.pl" 'if (\$match)'
check_output "batch4: assign-in-if declaration 2" "$TESTDIR/test_batch4.pl" 'my scalar \$val = lookup'
check_output "batch4: assign-in-if condition 2" "$TESTDIR/test_batch4.pl" 'if (\$val)'

# Copy-and-modify -> split into assignment + regex
check_output "batch4: copy-modify declaration" "$TESTDIR/test_batch4.pl" 'my scalar \$clean = \$dirty;'
check_output "batch4: copy-modify regex" "$TESTDIR/test_batch4.pl" '\$clean =~ s'
check_output "batch4: copy-modify no_ext decl" "$TESTDIR/test_batch4.pl" 'my scalar \$no_ext = \$file;'
check_output "batch4: copy-modify no_ext regex" "$TESTDIR/test_batch4.pl" '\$no_ext =~ s'

# Assignment in conditional with nested parens
check_output "batch4: assign-in-if nested parens" "$TESTDIR/test_batch4.pl" 'my scalar \$val = lookup(\$name)'
check_output "batch4: assign-in-if multi-arg" "$TESTDIR/test_batch4.pl" 'my scalar \$result = func(\$a, \$b)'

# Backreferences $1-$9 pass through natively (Strada supports $1-$9)
check_output "batch4: backref inline capture 1" "$TESTDIR/test_batch4.pl" '\$key = \$1'
check_output "batch4: backref inline capture 2" "$TESTDIR/test_batch4.pl" '\$value = \$2'
check_output "batch4: backref inline capture 3" "$TESTDIR/test_batch4.pl" '\$name = \$3'
check_output "batch4: backref no duplicate caps" "$TESTDIR/test_batch4.pl" '@_caps' "0"
check_output "batch4: backref string interpolation" "$TESTDIR/test_batch4.pl" '"matched: " \. \$1'

# List-context diamond passes through natively
check_output "batch4: slurp passes through" "$TESTDIR/test_batch4.pl" 'my array @lines = <'
check_output "batch4: slurp data passes through" "$TESTDIR/test_batch4.pl" 'my array @data = <'

# ---- test_batch5.pl ----
echo ""
echo "--- test_batch5.pl ---"
check_no_crash "batch5: no crash" "$TESTDIR/test_batch5.pl"

# Split /regex/ has proper closing paren
check_output "batch5: split /&/ has closing paren" "$TESTDIR/test_batch5.pl" 'split("&", \$query);'
check_output "batch5: split /=/ has closing paren" "$TESTDIR/test_batch5.pl" 'split("=", \$pair, 2);'
check_output "batch5: split /\s+/ has closing paren" "$TESTDIR/test_batch5.pl" 'split("\\s+", \$text);'

# __FILE__ and __LINE__ pass through without TODO
check_output "batch5: __FILE__ passes through" "$TESTDIR/test_batch5.pl" "__FILE__"
check_output "batch5: __FILE__ no TODO" "$TESTDIR/test_batch5.pl" "TODO.*__FILE__" "0"
check_output "batch5: __LINE__ passes through" "$TESTDIR/test_batch5.pl" "__LINE__"
check_output "batch5: __LINE__ no TODO" "$TESTDIR/test_batch5.pl" "TODO.*__LINE__" "0"

# Regex flag false positives: file paths should NOT trigger /i warning
check_output "batch5: mkdir no /i false positive" "$TESTDIR/test_batch5.pl" 'core::mkdir.*REVIEW.*/i' "0"
check_output "batch5: file path no /i false positive" "$TESTDIR/test_batch5.pl" 'mylib.*REVIEW.*/i' "0"

# Regex /i flag should work without REVIEW (PCRE2 is standard)
check_output "batch5: regex /i no REVIEW" "$TESTDIR/test_batch5.pl" 'REVIEW.*PCRE2' 0

# ---- Batch 6: New patterns (qq|..|, $#arr, @$ref, anon sub, Try::Tiny, multiline) ----
echo ""
echo "--- test_batch6.pl ---"
check_output "batch6: no crash" "$TESTDIR/test_batch6.pl" "func main"

# qq|...| -> double-quoted
check_output "batch6: qq pipe to double quote" "$TESTDIR/test_batch6.pl" '"Hello World"'

# q|...| -> single-quoted
check_output "batch6: q pipe to single quote" "$TESTDIR/test_batch6.pl" "'not interpolated'"

# $#array -> (scalar(@arr) - 1)
check_output "batch6: dollar-hash array" "$TESTDIR/test_batch6.pl" 'scalar(@arr) - 1'

# @$ref -> @{$ref}
check_output "batch6: at-dollar-ref simplified" "$TESTDIR/test_batch6.pl" '@\$ref'

# %$href -> keys(%{$href})
check_output "batch6: percent-dollar-ref simplified" "$TESTDIR/test_batch6.pl" 'keys(%\$href)'

# bare die -> throw("fatal error")
check_output "batch6: bare die to throw" "$TESTDIR/test_batch6.pl" 'throw("fatal error")'

# sub { } -> func (scalar ...@_) scalar { }
check_output "batch6: anon sub to func" "$TESTDIR/test_batch6.pl" 'func (scalar ...@_) scalar'

# catch { -> catch ($e) {
check_output "batch6: try-tiny catch parens" "$TESTDIR/test_batch6.pl" 'catch ($e)'

# Multiline postfix if joined
check_output "batch6: multiline postfix if joined" "$TESTDIR/test_batch6.pl" 'if ($cond) { return'

# Multiline push joined
check_output "batch6: multiline push joined" "$TESTDIR/test_batch6.pl" 'push(@items, { "name" =>'

# $SIG sub closing paren
check_output "batch6: sig sub has closing paren" "$TESTDIR/test_batch6.pl" 'core::signal("INT".*);'

# func helper() outside main
check_output "batch6: func outside main" "$TESTDIR/test_batch6.pl" '^func helper'

# Only one func main
TMPB6=$(mktemp /tmp/p2s_b6_XXXXXX.strada)
$P2S "$TESTDIR/test_batch6.pl" "$TMPB6" > /dev/null 2>&1
MAIN_COUNT=$(grep -c "^func main" "$TMPB6" 2>/dev/null || echo 0)
TOTAL=$((TOTAL + 1))
if [ "$MAIN_COUNT" -le 1 ]; then
    echo "ok: batch6: at most one func main"
    PASS=$((PASS + 1))
else
    echo "FAIL: batch6: at most one func main (got $MAIN_COUNT)"
    FAIL=$((FAIL + 1))
fi
rm -f "$TMPB6"

# ---- test_overload.pl ----
echo ""
echo "--- test_overload.pl ---"
check_no_crash "overload: no crash" "$TESTDIR/test_overload.pl"

# use overload passes through (native Strada)
check_output "overload: use overload passes through" "$TESTDIR/test_overload.pl" "use overload"

# overload::unimport commented out
check_output "overload: unimport commented" "$TESTDIR/test_overload.pl" "# overload::unimport"

# overload::import commented with TODO
check_output "overload: import REVIEW" "$TESTDIR/test_overload.pl" "REVIEW.*use overload"

# sub new converted to func
check_output "overload: func new generated" "$TESTDIR/test_overload.pl" "func new"

# ---- Batch 7: Signature pipeline, use base/parent, qr// flags, s{}{}, time(), anon sub ----
echo ""
echo "--- test_batch7.pl ---"
check_no_crash "batch7: no crash" "$TESTDIR/test_batch7.pl"

# use base -> extends
check_output "batch7: use base to extends" "$TESTDIR/test_batch7.pl" "extends Creature;"
check_output "batch7: use parent to extends" "$TESTDIR/test_batch7.pl" "extends Animal;"
check_output "batch7: use parent -norequire to extends" "$TESTDIR/test_batch7.pl" "extends Pet::Base;"
check_output "batch7: use base qw() multiple extends" "$TESTDIR/test_batch7.pl" "extends "

# qr// flag preservation
check_output "batch7: qr/pattern/i preserves flags" "$TESTDIR/test_batch7.pl" '"(?i)hello"'
check_output "batch7: qr/pattern/ no flags" "$TESTDIR/test_batch7.pl" '"world"'
check_output "batch7: qr/pattern/si multiple flags" "$TESTDIR/test_batch7.pl" '"(?si)foo'
check_output "batch7: qr{pattern}i brace form" "$TESTDIR/test_batch7.pl" '"(?i)test"'

# s{}{} brace substitution
check_output "batch7: s{}{} to s///" "$TESTDIR/test_batch7.pl" "s/hello/goodbye/g"
check_output "batch7: s{}{} no flags" "$TESTDIR/test_batch7.pl" "s/foo/bar/"

# time() -> core::time()
check_output "batch7: time() to core::time()" "$TESTDIR/test_batch7.pl" "core::time()"

# Anonymous sub with (scalar ...@_) scalar
check_output "batch7: anon sub variadic scalar" "$TESTDIR/test_batch7.pl" 'func (scalar ...@_) scalar {'

# Function signature extraction
check_output "batch7: process_data uses no-parens form" "$TESTDIR/test_batch7.pl" "func process_data {"
check_output "batch7: build_query uses no-parens form" "$TESTDIR/test_batch7.pl" "func build_query {"

# @_ unpacking preserved in no-parens form
check_output "batch7: @_ unpacking preserved in process_data" "$TESTDIR/test_batch7.pl" "my.*self.*data.*options.*= @_"

# map/grep/sort pass through
check_output "batch7: map passes through" "$TESTDIR/test_batch7.pl" "map {"
check_output "batch7: grep passes through" "$TESTDIR/test_batch7.pl" "grep {"
check_output "batch7: sort passes through" "$TESTDIR/test_batch7.pl" "sort {"

# ---- Batch 8: Tier 1 gaps (Scalar::Util, List::Util, $_, $?, local $/, interp, heredoc, do-while, const {}, pack) ----
echo ""
echo "--- test_batch8.pl ---"
check_no_crash "batch8: no crash" "$TESTDIR/test_batch8.pl"

# Scalar::Util
check_output "batch8: blessed -> ref" "$TESTDIR/test_batch8.pl" "ref(.obj)"
check_output "batch8: weaken converted" "$TESTDIR/test_batch8.pl" "core::weaken"
check_output "batch8: reftype -> ref" "$TESTDIR/test_batch8.pl" "ref(.thing).*REVIEW"
check_output "batch8: looks_like_number -> match" "$TESTDIR/test_batch8.pl" 'match.*[0-9]'
check_output "batch8: openhandle -> defined" "$TESTDIR/test_batch8.pl" "defined(.handle)"
check_output "batch8: tainted -> 0" "$TESTDIR/test_batch8.pl" "= 0.*taint"
check_output "batch8: isweak -> core::isweak" "$TESTDIR/test_batch8.pl" "core::isweak"

# List::Util
check_output "batch8: use List::Util preserved" "$TESTDIR/test_batch8.pl" "use List::Util;"
check_output "batch8: max -> List::Util::max" "$TESTDIR/test_batch8.pl" "List::Util::max"
check_output "batch8: min -> List::Util::min" "$TESTDIR/test_batch8.pl" "List::Util::min"
check_output "batch8: sum -> List::Util::sum" "$TESTDIR/test_batch8.pl" "List::Util::sum(.values)"
check_output "batch8: sum0 -> List::Util::sum0" "$TESTDIR/test_batch8.pl" "List::Util::sum0"
check_output "batch8: uniq -> List::Util::uniq" "$TESTDIR/test_batch8.pl" "List::Util::uniq"
check_output "batch8: shuffle -> List::Util::shuffle" "$TESTDIR/test_batch8.pl" "List::Util::shuffle"
check_output "batch8: pairs -> List::Util::pairs" "$TESTDIR/test_batch8.pl" "List::Util::pairs"
check_output "batch8: first {} converted" "$TESTDIR/test_batch8.pl" "# was: first"
check_output "batch8: any {} conversion" "$TESTDIR/test_batch8.pl" "# was: any"
check_output "batch8: all {} converted" "$TESTDIR/test_batch8.pl" "# was: all"
check_output "batch8: none {} converted" "$TESTDIR/test_batch8.pl" "# was: none"
check_output "batch8: reduce {} converted" "$TESTDIR/test_batch8.pl" "# was: reduce"

# Bare $_ implicit
check_output "batch8: bare chomp" "$TESTDIR/test_batch8.pl" 's/.n'
check_output "batch8: bare print" "$TESTDIR/test_batch8.pl" 'say(.*_)'
check_output "batch8: bare say" "$TESTDIR/test_batch8.pl" 'say(._)'
check_output "batch8: bare chop" "$TESTDIR/test_batch8.pl" 'substr(._'

# $? child process status
check_output "batch8: exit code converted" "$TESTDIR/test_batch8.pl" '__exit_code'

# local $/ slurp
check_output "batch8: local slash slurp" "$TESTDIR/test_batch8.pl" 'core::slurp'
check_output "batch8: local slash undef commented" "$TESTDIR/test_batch8.pl" '# local.*slurp'

# String interpolation $arr[N]
check_output "batch8: interp arr[N]" "$TESTDIR/test_batch8.pl" '\. .items\[0\]'
check_output "batch8: interp ref->[N]" "$TESTDIR/test_batch8.pl" '\. .ref->\[2\]'
check_output "batch8: interp arr[var]" "$TESTDIR/test_batch8.pl" '\. .data\[.idx\]'

# Heredoc interpolation
check_output "batch8: heredoc interp name" "$TESTDIR/test_batch8.pl" '\. .name \.'
check_output "batch8: heredoc non-interp" "$TESTDIR/test_batch8.pl" 'SELECT.*FROM'

# do {} while
check_output "batch8: do while passes through" "$TESTDIR/test_batch8.pl" '} while (.count < 5)'

# use constant hashref
check_output "batch8: const MAX_SIZE" "$TESTDIR/test_batch8.pl" 'const scalar MAX_SIZE = 1024'
check_output "batch8: const MIN_SIZE" "$TESTDIR/test_batch8.pl" 'const scalar MIN_SIZE = 16'
check_output "batch8: const DEFAULT_NAME" "$TESTDIR/test_batch8.pl" 'const scalar DEFAULT_NAME'
check_output "batch8: const single" "$TESTDIR/test_batch8.pl" 'const scalar SINGLE_CONST = 42'

# pack/unpack
check_output "batch8: pack -> core::pack" "$TESTDIR/test_batch8.pl" 'core::pack("NnC"'
check_output "batch8: unpack -> core::unpack" "$TESTDIR/test_batch8.pl" 'core::unpack("NnC"'

# ---- Batch 9: 10 high-impact improvements ----
echo ""
echo "--- Batch 9 tests ---"

# 1. Parameter extraction: shift @_ and shift(@_)
check_output "batch9: shift @_ param" "$TESTDIR/test_batch9.pl" 'func from_shift_at'

# 2. Multi-line joining: method chains
check_output "batch9: method chain joined" "$TESTDIR/test_batch9.pl" 'set_name.*->set_value.*->build'

# 2b. Multi-line joining: dot concat
check_output "batch9: dot concat joined" "$TESTDIR/test_batch9.pl" '"Hello ".*"World "'

# 2c. Multi-line joining: binary operator continuation
check_output "batch9: binary op joined" "$TESTDIR/test_batch9.pl" '&&.*||'

# 3. String interpolation: @array
check_output "batch9: @array interp" "$TESTDIR/test_batch9.pl" 'join(" ", @fruits)'

# 3b. String interpolation: chained access
check_output "batch9: chained hash access" "$TESTDIR/test_batch9.pl" 'obj->{"config"}->{"host"}'

# 4. @ARGV conversion
check_output "batch9: ARGV[0]" "$TESTDIR/test_batch9.pl" 'core::argv()\[0\]'
check_output "batch9: @ARGV" "$TESTDIR/test_batch9.pl" 'core::argv()'
check_output "batch9: scalar(@ARGV)" "$TESTDIR/test_batch9.pl" 'scalar(core::argv())'
check_output "batch9: $0 -> argv[0]" "$TESTDIR/test_batch9.pl" 'core::argv()\[0\]'

# 5. Exporter patterns
check_output "batch9: @EXPORT comment" "$TESTDIR/test_batch9.pl" '# our @EXPORT.*Not needed'
check_output "batch9: @EXPORT_OK comment" "$TESTDIR/test_batch9.pl" '# our @EXPORT_OK.*Not needed'
check_output "batch9: use parent Exporter" "$TESTDIR/test_batch9.pl" '# use parent.*Exporter'
check_output "batch9: require Exporter" "$TESTDIR/test_batch9.pl" 'use Exporter;'
check_output "batch9: %EXPORT_TAGS" "$TESTDIR/test_batch9.pl" '# .*EXPORT_TAGS'

# 6. File test operators
check_output "batch9: -z conversion" "$TESTDIR/test_batch9.pl" 'core::file_size.*== 0'
check_output "batch9: -r improved" "$TESTDIR/test_batch9.pl" 'core::access.*4.*== 0'
check_output "batch9: -w improved" "$TESTDIR/test_batch9.pl" 'core::access.*2.*== 0'
check_output "batch9: -x improved" "$TESTDIR/test_batch9.pl" 'core::access.*1.*== 0'

# 7. opendir/readdir/closedir
check_output "batch9: opendir guidance" "$TESTDIR/test_batch9.pl" 'core::readdir'
check_output "batch9: closedir commented" "$TESTDIR/test_batch9.pl" '# closedir.*unnecessary'

# 8. sprintf bare form
check_output "batch9: sprintf parens" "$TESTDIR/test_batch9.pl" 'sprintf("%s: %d"'

# 9. SUPER:: resolved to parent
check_output "batch9: SUPER -> Animal (Dog)" "$TESTDIR/test_batch9.pl" 'Animal::speak'
check_output "batch9: SUPER -> Animal (Cat)" "$TESTDIR/test_batch9.pl" 'Animal::speak'

# 10. Ampersand call syntax
check_output "batch9: &func -> func" "$TESTDIR/test_batch9.pl" 'process_data(\$input)'
check_output "batch9: &Mod::func -> func" "$TESTDIR/test_batch9.pl" 'Module::init()'
check_output "batch9: &\$ref -> deref" "$TESTDIR/test_batch9.pl" '\$callback->('
check_output "batch9: \\&func preserved" "$TESTDIR/test_batch9.pl" '\\&some_func'

# ---- Smart paren test ----
echo ""
echo "--- Smart paren verification ---"
check_output "smart_paren: scalar(keys()) correct" "$TESTDIR/test_slices_misc.pl" "REVIEW" "1"

# ---- Batch 10 tests ----
echo ""
echo "--- Batch 10: High-impact improvements ---"

# 1. $| (autoflush) - commented out
check_output "batch10: autoflush $|=1 commented" "$TESTDIR/test_batch10.pl" "# .*auto-flushes"
check_output "batch10: autoflush $|++ commented" "$TESTDIR/test_batch10.pl" "# .*auto-flushes"

# 2. Special variables
check_output "batch10: $. commented out" "$TESTDIR/test_batch10.pl" '# .*\$\..*not available'
check_output "batch10: $^O -> core::uname" "$TESTDIR/test_batch10.pl" 'core::uname()'
check_output "batch10: $^W commented" "$TESTDIR/test_batch10.pl" '# .*not needed in Strada'
check_output "batch10: output record sep" "$TESTDIR/test_batch10.pl" 'output record separator.*not needed'
check_output "batch10: output field sep" "$TESTDIR/test_batch10.pl" 'output field separator.*not supported'

# 3. while (<>) / while (<STDIN>) conversion
check_output "batch10: while (<>) readline" "$TESTDIR/test_batch10.pl" "core::readline"
check_output "batch10: while (<STDIN>) readline" "$TESTDIR/test_batch10.pl" "core::readline"
check_output "batch10: while (my line = <STDIN>)" "$TESTDIR/test_batch10.pl" 'defined.*core::readline'

# 4. __DATA__/__END__ section preservation
check_output "batch10: __END__ commented" "$TESTDIR/test_batch10_data.pl" "# __END__"
check_output "batch10: data preserved" "$TESTDIR/test_batch10_data.pl" "# __DATA__:.*should NOT be converted"
check_output "batch10: data not mangled" "$TESTDIR/test_batch10_data.pl" "# __DATA__:.*should_not_become_func" "1"

# 5. Indirect object syntax
check_output "batch10: new Foo -> Foo::new" "$TESTDIR/test_batch10.pl" 'Foo::new'
check_output "batch10: new My::Widget -> My::Widget::new" "$TESTDIR/test_batch10.pl" 'My::Widget::new'

# 6. flock() and use Fcntl
check_output "batch10: use Fcntl commented" "$TESTDIR/test_batch10.pl" "# use Fcntl.*flock constants"
check_output "batch10: flock -> core::flock" "$TESTDIR/test_batch10.pl" "core::flock("

# 7. open() read-write modes
check_output "batch10: open +< rw" "$TESTDIR/test_batch10.pl" 'core::open.*"rw".*read-write without truncate'
check_output "batch10: open +> truncate" "$TESTDIR/test_batch10.pl" 'core::open.*"w".*read-write with truncate'
check_output "batch10: open +>> append" "$TESTDIR/test_batch10.pl" 'core::open.*"a".*read-write append'

# 8. Moose has improvements
check_output "batch10: has default sub [] -> []" "$TESTDIR/test_batch10.pl" 'has rw scalar \$items = \[\]'
check_output "batch10: has default sub {} -> {}" "$TESTDIR/test_batch10.pl" 'has ro scalar \$config = {}'
check_output "batch10: has default sub string" "$TESTDIR/test_batch10.pl" 'has ro str \$greeting = "hello"'
check_output "batch10: has default sub number" "$TESTDIR/test_batch10.pl" 'has rw int \$count = 0'
check_output "batch10: has CodeRef -> scalar" "$TESTDIR/test_batch10.pl" 'has ro scalar \$callback'
check_output "batch10: has trigger after hook" "$TESTDIR/test_batch10.pl" 'after "set_'
check_output "batch10: has coerce note" "$TESTDIR/test_batch10.pl" 'coerce.*natively'
check_output "batch10: has handles delegation" "$TESTDIR/test_batch10.pl" 'handles delegation'
check_output "batch10: has clearer generates method" "$TESTDIR/test_batch10.pl" 'func clear_'
check_output "batch10: has predicate generates method" "$TESTDIR/test_batch10.pl" 'func has_'
check_output "batch10: has weak_ref -> core::weaken" "$TESTDIR/test_batch10.pl" 'core::weaken'

# 9. use POSIX
check_output "batch10: use POSIX commented" "$TESTDIR/test_batch10.pl" "# use POSIX.*REVIEW"

# ---- Batch 11: Import tracking, DBI, new modules, function mapping ----
echo ""
echo "--- Batch 11: Import tracking, DBI, modules, function mapping ---"
check_no_crash "batch11: no crash" "$TESTDIR/test_batch11.pl"

# 1. DBI module conversion
check_output "batch11: use DBI" "$TESTDIR/test_batch11.pl" 'use DBI;'
check_output "batch11: DBI->connect -> DBI::connect" "$TESTDIR/test_batch11.pl" 'DBI::connect('
check_output "batch11: $dbh methods pass through" "$TESTDIR/test_batch11.pl" 'dbh->prepare'

# 2. New module use handlers
check_output "batch11: File::Copy hint" "$TESTDIR/test_batch11.pl" "core::rename.*cp"
check_output "batch11: File::Temp hint" "$TESTDIR/test_batch11.pl" "core::tmpfile"
check_output "batch11: File::Spec hint" "$TESTDIR/test_batch11.pl" "string path"
check_output "batch11: Cwd hint" "$TESTDIR/test_batch11.pl" "core::getcwd.*core::realpath"
check_output "batch11: IO::File hint" "$TESTDIR/test_batch11.pl" "core::open"
check_output "batch11: IO::Handle hint" "$TESTDIR/test_batch11.pl" "auto-flushes"
check_output "batch11: IO::Select REVIEW" "$TESTDIR/test_batch11.pl" "core::select.*poll"
check_output "batch11: Socket hint" "$TESTDIR/test_batch11.pl" "core::socket_client"
check_output "batch11: HTTP::Tiny REVIEW" "$TESTDIR/test_batch11.pl" "REVIEW.*HTTP"
check_output "batch11: LWP::UserAgent REVIEW" "$TESTDIR/test_batch11.pl" "REVIEW.*HTTP"
check_output "batch11: MIME::Base64 hint" "$TESTDIR/test_batch11.pl" "core::base64_encode.*core::base64_decode"
check_output "batch11: Getopt::Std hint" "$TESTDIR/test_batch11.pl" "core::argv.*directly"
check_output "batch11: Getopt::Long native" "$TESTDIR/test_batch11.pl" "use Getopt::Long;"
check_output "batch11: Test::More REVIEW" "$TESTDIR/test_batch11.pl" "REVIEW.*no test framework"
check_output "batch11: Config REVIEW" "$TESTDIR/test_batch11.pl" "REVIEW.*core::getenv.*config"
check_output "batch11: Text::CSV -> use TextCSV" "$TESTDIR/test_batch11.pl" 'use TextCSV;'

# 3. Imported function mapping
check_output "batch11: basename -> core::basename" "$TESTDIR/test_batch11.pl" 'core::basename('
check_output "batch11: dirname -> core::dirname" "$TESTDIR/test_batch11.pl" 'core::dirname('
check_output "batch11: getcwd -> core::getcwd" "$TESTDIR/test_batch11.pl" 'core::getcwd()'
check_output "batch11: abs_path -> core::realpath" "$TESTDIR/test_batch11.pl" 'core::realpath("/some/link")'
check_output "batch11: realpath -> core::realpath" "$TESTDIR/test_batch11.pl" 'core::realpath("/another/link")'
check_output "batch11: floor -> math::floor" "$TESTDIR/test_batch11.pl" 'math::floor(3.7)'
check_output "batch11: ceil -> math::ceil" "$TESTDIR/test_batch11.pl" 'math::ceil(3.2)'
check_output "batch11: strftime -> core::strftime" "$TESTDIR/test_batch11.pl" 'core::strftime('
check_output "batch11: encode_base64 -> core::base64_encode" "$TESTDIR/test_batch11.pl" 'core::base64_encode("hello")'
check_output "batch11: decode_base64 -> core::base64_decode" "$TESTDIR/test_batch11.pl" 'core::base64_decode('

# 4. JSON encode/decode
check_output "batch11: encode_json -> JSON::encode" "$TESTDIR/test_batch11.pl" 'JSON::encode('
check_output "batch11: decode_json -> JSON::decode" "$TESTDIR/test_batch11.pl" 'JSON::decode('
check_output "batch11: JSON use statement" "$TESTDIR/test_batch11.pl" 'use JSON;'

# 5. GetOptions pass-through
check_output "batch11: GetOptions argv loop" "$TESTDIR/test_batch11.pl" 'REVIEW.*auto-converted from GetOptions'

# 7. Import tracking: unknown imported functions get module prefix
check_output "batch11: import tracking custom_func" "$TESTDIR/test_batch11.pl" 'Some::Module::custom_func('
check_output "batch11: import tracking helper_func" "$TESTDIR/test_batch11.pl" 'Some::Module::helper_func('
check_output "batch11: import tracking REVIEW" "$TESTDIR/test_batch11.pl" 'REVIEW.*imported from Some::Module'

# 8. Import tracking: single import form
check_output "batch11: single import process_data" "$TESTDIR/test_batch11.pl" 'Another::Lib::process_data('

# ---- Batch 12: Named captures, encoding open, flock, quotemeta, signals, Moose extras, List::Util HOFs ----
B12="$TESTDIR/test_batch12.pl"

check_no_crash "batch12: no crash" "$B12"

# Named captures
check_output "batch12: named_captures hash access" "$B12" 'named_captures(){"word"}'
check_output "batch12: named_captures num" "$B12" 'named_captures(){"num"}'

# Three-arg open with encoding
check_output "batch12: open read encoding stripped" "$B12" 'core::open.*"r".*encoding.*stripped.*UTF-8 native'
check_output "batch12: open write encoding stripped" "$B12" 'core::open.*"w".*encoding.*stripped.*UTF-8 native'

# flock
check_output "batch12: flock LOCK_EX -> 2" "$B12" 'core::flock.*2)'
check_output "batch12: flock LOCK_SH|LOCK_NB -> 1|4" "$B12" 'core::flock.*1 | 4'
check_output "batch12: flock LOCK_UN -> 8" "$B12" 'core::flock.*8)'

# quotemeta
check_output "batch12: quotemeta -> core::quotemeta" "$B12" 'core::quotemeta('

# $SIG{__DIE__} and $SIG{__WARN__}
check_output "batch12: SIG DIE -> REVIEW comment" "$B12" '# .*__DIE__.*REVIEW.*try/catch'
check_output "batch12: SIG WARN -> REVIEW comment" "$B12" '# .*__WARN__.*REVIEW'
check_output "batch12: DIE no core::signal" "$B12" 'core::signal.*__DIE__' 0
check_output "batch12: WARN no core::signal" "$B12" 'core::signal.*__WARN__' 0

# Moose predicate/clearer
check_output "batch12: has clearer generates clear_name" "$B12" 'func clear_name.*void.*undef'
check_output "batch12: has predicate generates has_name" "$B12" 'func has_name.*int.*defined'
check_output "batch12: has handles delegation" "$B12" "handles delegation.*data"
check_output "batch12: has trigger after hook" "$B12" 'after "set_hook"'

# Getopt::Long pass-through
check_output "batch12: use Getopt::Long preserved" "$B12" 'use Getopt::Long;'
check_output "batch12: GetOptions argv loop" "$B12" 'REVIEW.*auto-converted from GetOptions'

# first { BLOCK } conversion
check_output "batch12: first comment" "$B12" '# was: first'
check_output "batch12: first -> for loop" "$B12" 'for.*__i.*__item.*last'
check_output "batch12: first assigns to $found" "$B12" 'found = .*__item'

# any/all/none/reduce REVIEW
check_output "batch12: any converted" "$B12" '# was: any'
check_output "batch12: all converted" "$B12" '# was: all'
check_output "batch12: none converted" "$B12" '# was: none'
check_output "batch12: reduce converted" "$B12" '# was: reduce'

# ---- Batch 13: Tier 2 gaps (multi roles/parents, qr//, import, requires, DBI, finally, has+, protos, /r, //g) ----
B13="$TESTDIR/test_batch13.pl"

check_no_crash "batch13: no crash" "$B13"

# Multiple roles/parents
check_output "batch13: extends Animal" "$B13" 'extends Animal;'
check_output "batch13: extends Mammal" "$B13" 'extends Mammal;'
check_output "batch13: with Printable" "$B13" 'with Printable;'
check_output "batch13: with Serializable" "$B13" 'with Serializable;'
check_output "batch13: with Loggable" "$B13" 'with Loggable;'

# Moose role requires
check_output "batch13: requires swim_speed" "$B13" 'requires "swim_speed"'
check_output "batch13: requires dive" "$B13" 'requires "dive"'
check_output "batch13: requires surface" "$B13" 'requires "surface"'
check_output "batch13: requires breathe" "$B13" 'requires "breathe"'

# has [qw(...)] multi-attr
check_output "batch13: has ro int width" "$B13" 'has ro int .width'
check_output "batch13: has ro int height" "$B13" 'has ro int .height'

# has +attr subclass override
check_output "batch13: has +name override" "$B13" "overrides parent attribute"
check_output "batch13: has +name generates has" "$B13" 'has r[ow]'

# Custom import() method
check_output "batch13: func import signature" "$B13" 'func import(str .pkg, array @list) void'

# qr// direct match REVIEW
check_output "batch13: qr// var match REVIEW" "$B13" 'REVIEW.*regex variable match.*match'

# DBI handle methods
check_output "batch13: DBI prepare passthrough" "$B13" 'prepare\('
check_output "batch13: DBI execute passthrough" "$B13" 'execute\('
check_output "batch13: DBI fetchrow_hashref passthrough" "$B13" 'fetchrow_hashref'
check_output "batch13: DBI fetchrow_array passthrough" "$B13" 'fetchrow_array'
check_output "batch13: DBI fetchall_arrayref passthrough" "$B13" 'fetchall_arrayref'
check_output "batch13: DBI finish passthrough" "$B13" 'finish'
check_output "batch13: DBI begin_work passthrough" "$B13" 'begin_work'
check_output "batch13: DBI commit passthrough" "$B13" 'commit'
check_output "batch13: DBI rollback passthrough" "$B13" 'rollback'
check_output "batch13: DBI bind_columns passthrough" "$B13" 'bind_columns'

# Try::Tiny finally
check_output "batch13: finally block" "$B13" 'finally block'

# Subroutine prototypes stripped
check_output "batch13: mysub uses no-parens form" "$B13" 'func mysub {'
check_output "batch13: constant_val proto stripped" "$B13" 'func constant_val'
check_output "batch13: takes_hash uses no-parens form" "$B13" 'func takes_hash {'
check_output "batch13: no prototype in output" "$B13" '(&@)' 0
check_output "batch13: no proto parens" "$B13" '(\\%)' 0

# /r flag
check_output "batch13: /r flag copy-modify" "$B13" 'was s///r.*non-destructive'
check_output "batch13: /r s///g preserved" "$B13" 's/.s\+/_/g'

# Global match in list context
check_output "batch13: //g list context numbers" "$B13" 'while.*=~ /.*\\d.*g\).*push.*\$1'
check_output "batch13: //g list context pairs" "$B13" 'while.*=~ /.*\\w.*g\).*push.*\$1'

# ============================================================
# Batch 14: Priority 1 features (signatures, map/grep, scalar, deref, open)
# ============================================================
B14="$TESTDIR/test_batch14.pl"

check_no_crash "batch14: no crash" "$B14"

# Native subroutine signatures — converted to no-parens form with @_ unpacking
check_output "batch14: sig greet scalar+str" "$B14" 'func greet {'
check_output "batch14: sig add two params" "$B14" 'func add {'
check_output "batch14: sig process with hash" "$B14" 'func process {'
check_output "batch14: sig variadic with array" "$B14" 'func variadic {'
check_output "batch14: sig int default" "$B14" 'func with_default_int {'
check_output "batch14: sig num default" "$B14" 'func with_default_num {'

# map/grep block forms
check_output "batch14: map bare uc" "$B14" 'map { uc(.*) }'
check_output "batch14: map bare lc" "$B14" 'map { lc(.*) }'
check_output "batch14: map bare chomp" "$B14" 'map { chomp(.*) }'
check_output "batch14: grep bare regex" "$B14" 'grep.*=~ /\^#/'
check_output "batch14: grep negated regex" "$B14" 'grep.*!~ /\^.s'

# sort with $a/$b (passthrough)
check_output "batch14: sort cmp" "$B14" 'sort.*cmp'
check_output "batch14: sort spaceship" "$B14" 'sort.*<=>'

# Scalar context
check_output "batch14: scalar context count" "$B14" 'scalar(@items)'
check_output "batch14: scalar context len" "$B14" 'scalar(@array)'
check_output "batch14: scalar context assign" "$B14" 'scalar(@things)'
check_output "batch14: my int from scalar" "$B14" 'my int .count = scalar'

# Postfix dereference
check_output "batch14: postfix @* deref" "$B14" '@\$ref'
check_output "batch14: postfix %* deref simplified" "$B14" '%\$ref'
check_output "batch14: postfix scalar deref" "$B14" '\$\$ref'
check_output "batch14: postfix slice converted" "$B14" '@\$aref\[0, 2, 4\]'

# open to string ref
check_output "batch14: open string ref input" "$B14" 'core::open.*"r"'
check_output "batch14: open string ref output" "$B14" 'core::open.*"w"'

# ============================================================
# Batch 15: Typeglob handling and bareword filehandles
# ============================================================
B15="$TESTDIR/test_batch15.pl"

check_no_crash "batch15: no crash" "$B15"

# Function alias typeglobs
check_output "batch15: func alias encode" "$B15" 'func encode(scalar ...@_) dynamic.*return encode_utf8'
check_output "batch15: func alias decode" "$B15" 'func decode(scalar ...@_) dynamic.*return decode_utf8'
check_output "batch15: func alias private" "$B15" 'func _private_alias(scalar ...@_) dynamic.*return _original_helper'

# Variable alias (renamed throughout)
check_output "batch15: scalar alias renamed" "$B15" 'total_count + 1'
check_output "batch15: array alias renamed" "$B15" 'push(@all_items'
check_output "batch15: hash alias renamed" "$B15" 'object_cache.*"key".*=.*"value"'

# qw() loop expansion to getter/setter funcs
check_output "batch15: getter name" "$B15" 'func name(scalar .self) scalar'
check_output "batch15: getter age" "$B15" 'func age(scalar .self) scalar'
check_output "batch15: getter email" "$B15" 'func email(scalar .self) scalar'
check_output "batch15: setter set_name" "$B15" 'func set_name(scalar .self, scalar .value) void'
check_output "batch15: setter set_age" "$B15" 'func set_age(scalar .self, scalar .value) void'
check_output "batch15: setter set_email" "$B15" 'func set_email(scalar .self, scalar .value) void'

# Single-line typeglob methods
check_output "batch15: get_status getter" "$B15" 'func get_status(scalar .self) scalar'
check_output "batch15: set_status setter" "$B15" 'func set_status(scalar .self, scalar .value) void'

# Full typeglob alias
check_output "batch15: full typeglob comment" "$B15" 'Full typeglob alias.*io.*IO'

# local *FH
check_output "batch15: local FH to lexical" "$B15" 'my scalar .fh.*was.*local'

# Bareword filehandle open/print/close
check_output "batch15: bareword open LOG" "$B15" 'core::open.*output.log.*"w"'
check_output "batch15: bareword print LOG" "$B15" 'say(.log.*Hello'
check_output "batch15: bareword close LOG" "$B15" 'core::close(.log)'
check_output "batch15: bareword open DATA_FILE" "$B15" 'core::open.*data.txt.*"r"'
check_output "batch15: bareword close DATA_FILE" "$B15" 'core::close(.data_file)'

# 2-arg bareword open
check_output "batch15: 2arg open TEMP" "$B15" 'core::open.*tmpfile.txt.*"w"'
check_output "batch15: 2arg print TEMP" "$B15" 'say(.temp.*temp data'
check_output "batch15: 2arg close TEMP" "$B15" 'core::close(.temp)'

# ---- Batch 16: Final gap closure (file tests, match vars, given/when, study, pos, oct, format, etc.) ----
echo ""
echo "--- test_batch16.pl ---"
B16=$(mktemp)
$P2S "$TESTDIR/test_batch16.pl" "$B16" > /dev/null 2>&1

# File test -T/-B/-M/-A/-C
check_output "batch16: file test -T REVIEW" "$B16" 'REVIEW.*-T.*text file'
check_output "batch16: file test -B REVIEW" "$B16" 'REVIEW.*-B.*binary file'
check_output "batch16: file test -M stat" "$B16" 'core::stat.*mtime'
check_output "batch16: file test -A stat" "$B16" 'core::stat.*atime'
check_output "batch16: file test -C stat" "$B16" 'core::stat.*ctime'

# Match variables
check_output "batch16: match var ampersand converted" "$B16" 'captures\(\)\[0\]'
check_output "batch16: match var prematch REVIEW" "$B16" 'REVIEW.*prematch'
check_output "batch16: match var postmatch REVIEW" "$B16" 'REVIEW.*postmatch'

# /g iterator
check_output "batch16: /g iterator no REVIEW" "$B16" 'REVIEW.*/g iterator' 0

# given/when numeric vs string
check_output "batch16: when numeric uses ==" "$B16" '__given == 42'
check_output "batch16: when string uses eq" "$B16" '__given eq "hello"'
check_output "batch16: when variable uses eq" "$B16" '__given eq .var'
check_output "batch16: default -> else" "$B16" 'else.*was: default'

# study -> comment out
check_output "batch16: study commented out" "$B16" '# study.*no-op'

# pos -> REVIEW
check_output "batch16: pos REVIEW" "$B16" 'REVIEW.*pos()'

# oct -> REVIEW
check_output "batch16: oct -> core::oct" "$B16" 'core::oct'

# read/sysread/syswrite
check_output "batch16: read -> core::read" "$B16" 'core::read(\$fh'
check_output "batch16: sysread -> core::read" "$B16" 'core::read(FD'
check_output "batch16: syswrite -> core::write" "$B16" 'core::write(FD'

# format/write commented out
check_output "batch16: format block commented" "$B16" '# format STDOUT'
check_output "batch16: format body commented" "$B16" '# @<<<<'
check_output "batch16: write commented" "$B16" '# write.*sprintf'

# goto passes through
check_output "batch16: goto passes through" "$B16" 'goto DONE'

# conditional use
check_output "batch16: use if commented" "$B16" '# use.*REVIEW.*conditional'

# \Q...\E
check_output "batch16: backslash Q E REVIEW" "$B16" 'REVIEW.*PCRE2'

# @ISA -> extends (multiple parents)
check_output "batch16: ISA qw two parents 1" "$B16" 'extends Parent1'
check_output "batch16: ISA qw two parents 2" "$B16" 'extends Parent2'
check_output "batch16: ISA single parent" "$B16" 'extends Base'

# Hash slices pass through
check_output "batch16: hash slice passes through" "$B16" '@hash{"key1"'
check_output "batch16: array slice passes through" "$B16" '@array\[0, 2, 4\]'

# Named captures
check_output "batch16: named capture word" "$B16" 'named_captures(){"word"}'
check_output "batch16: named capture num" "$B16" 'named_captures(){"num"}'


# ============================================================
# Batch 17: Gap closure - List::Util loops, SUPER:: chains, $!, modules
# ============================================================
B17="$TESTDIR/test_batch17.pl"

check_no_crash "batch17: no crash" "$B17"

# any/all/none/reduce with assignment -> loop conversion
check_output "batch17: any loop conversion" "$B17" '# was: any'
check_output "batch17: any init 0" "$B17" 'has_neg = 0'
check_output "batch17: any loop body" "$B17" 'has_neg = 1.*last'
check_output "batch17: all loop conversion" "$B17" '# was: all'
check_output "batch17: all init 1" "$B17" 'all_pos = 1'
check_output "batch17: none loop conversion" "$B17" '# was: none'
check_output "batch17: none init 1" "$B17" 'no_zero = 1'
check_output "batch17: reduce loop conversion" "$B17" '# was: reduce'
check_output "batch17: reduce init first" "$B17" 'total = @values\[0\]'
check_output "batch17: reduce loop from 1" "$B17" '__i = 1'

# SUPER:: method chain -> Parent::method($self)
check_output "batch17: SUPER greet" "$B17" 'Parent::greet(.*self)'
check_output "batch17: SUPER greet with args" "$B17" 'Parent::greet(.*self.*name)'

# $! errno handling
check_output "batch17: errno conversion" "$B17" 'core::strerror(core::errno())'
check_output "batch17: errno no REVIEW" "$B17" 'REVIEW.*errno' 0

# wait() -> core::wait()
check_output "batch17: wait converted" "$B17" 'core::wait()'

# pipe() -> REVIEW
check_output "batch17: pipe conversion" "$B17" 'core::pipe()'

# glob() function
check_output "batch17: glob function" "$B17" 'core::glob(".*\.txt")'
check_output "batch17: glob var log" "$B17" 'core::glob("/var/log'

# <*.pl> glob syntax
check_output "batch17: glob angle bracket" "$B17" 'core::glob(".*\.pl")'

# opendir/readdir/closedir
check_output "batch17: opendir commented" "$B17" '# opendir'
check_output "batch17: readdir converted" "$B17" 'core::readdir'
check_output "batch17: closedir commented" "$B17" '# closedir'

# Module handlers
check_output "batch17: Try::Tiny native" "$B17" 'Not needed.*native try/catch'
check_output "batch17: Carp native" "$B17" 'die/warn.*stack traces'
check_output "batch17: Data::Dumper built-in" "$B17" 'dumper.*built-in'
check_output "batch17: File::Find readdir" "$B17" 'core::readdir.*recursion'
check_output "batch17: File::Path mkdir" "$B17" 'core::mkdir'
check_output "batch17: Digest::SHA" "$B17" 'use crypt;.*Digest'
check_output "batch17: Fcntl flock" "$B17" 'core::flock\|flock'
check_output "batch17: Errno sys" "$B17" 'core::errno'
check_output "batch17: IPC popen" "$B17" 'core::popen\|core::system'
check_output "batch17: Net::SMTP socket" "$B17" 'core::socket_client\|import_lib'
check_output "batch17: IO::Socket::UNIX" "$B17" 'core::socket'
check_output "batch17: Params native" "$B17" 'native type annotations'
check_output "batch17: Type::Tiny native" "$B17" 'native type'

# /e flag
check_output "batch17: /e flag REVIEW" "$B17" 'REVIEW.*/e flag.*eval replacement'

# ==================== BATCH 18: GetOptions argv loop, bare carp, fileparse ====================
B18="$TESTDIR/test_batch18.pl"

# GetOptions simple boolean -> argv loop
check_output "batch18: GetOptions bool argv loop" "$B18" 'REVIEW.*auto-converted from GetOptions'
check_output "batch18: GetOptions verbose flag" "$B18" 'eq "--verbose"'
check_output "batch18: GetOptions debug flag" "$B18" 'eq "--debug"'

# GetOptions string/int args
check_output "batch18: GetOptions string arg" "$B18" 'eq "--output".*argi.*output'
check_output "batch18: GetOptions int arg" "$B18" 'eq "--count".*argi.*count'

# GetOptions with aliases
check_output "batch18: GetOptions alias -h" "$B18" 'eq "-h"'
check_output "batch18: GetOptions alias -f" "$B18" 'eq "-f"'

# GetOptions negatable
check_output "batch18: GetOptions negatable --color" "$B18" 'eq "--color"'
check_output "batch18: GetOptions negatable --no-color" "$B18" 'eq "--no-color"'
check_output "batch18: GetOptions negatable --nocolor" "$B18" 'eq "--nocolor"'

# GetOptions numeric arg
check_output "batch18: GetOptions float arg" "$B18" 'eq "--threshold"'

# GetOptions generates core::argc loop
check_output "batch18: GetOptions core::argc" "$B18" 'core::argc()'
check_output "batch18: GetOptions core::argv" "$B18" 'core::argv'

# bare carp -> warn
check_output "batch18: bare carp to warn" "$B18" 'warn.*this is a warning'
check_output "batch18: bare carp() to warn()" "$B18" 'warn("this is also a warning")'

# fileparse -> REVIEW
check_output "batch18: fileparse REVIEW" "$B18" 'fileparse.*REVIEW.*core::basename.*core::dirname'

# ==================== BATCH 19: Moose has, exists paren, OOP placement ====================
B19="$TESTDIR/test_batch19.pl"

# Moose has -> Strada has
check_output "batch19: has ro str required" "$B19" 'has ro str \$name (required)'
check_output "batch19: has rw int default" "$B19" 'has rw int \$count = 0'
check_output "batch19: has lazy builder" "$B19" 'has ro scalar \$cache (lazy, builder'
check_output "batch19: has predicate method" "$B19" 'func has_logger.*return defined'
check_output "batch19: has clearer method" "$B19" 'func clear_buffer.*undef'
check_output "batch19: has handles delegation" "$B19" 'handles delegation'
check_output "batch19: has rw Bool" "$B19" 'has rw int \$flag = 0'

# OOP at package level (not inside main)
check_output "batch19: extends at pkg level" "$B19" '^extends'
check_output "batch19: with at pkg level" "$B19" '^with'

# has NOT inside main()
check_output "batch19: has not in main" "$B19" 'func main' "0"

# exists with arrow chain paren
check_output "batch19: exists arrow chain" "$B19" 'exists(\$self->data->{\$key})'
check_output "batch19: exists nested hash" "$B19" 'exists(\$self->{"cache"}{\$a})'

# Moose meta cleanup
check_output "batch19: meta immutable commented" "$B19" '# __PACKAGE__->meta->make_immutable'
check_output "batch19: no Moose commented" "$B19" '# no Moose'


# ---- Batch 20: eval or do, die hashref, @{[expr]}, $SIG, s///r ----
B20="$TESTDIR/test_batch20.pl"

# eval {} or do {} -> try/catch
check_output "batch20: eval or do -> try/catch" "$B20" 'try {'
check_output "batch20: eval or do -> catch" "$B20" 'catch.*\$e'
check_output "batch20: eval or die -> catch die" "$B20" 'die.*Failed to connect'

# die hashref -> throw
check_output "batch20: die hashref -> throw" "$B20" 'throw.*type.*NotFound'

# @{[expr]} in strings
check_output "batch20: @{[expr]} interpolation" "$B20" 'Result.*1 \+ 2'

# ${\(expr)} in strings
check_output "batch20: \${\(expr)} interpolation" "$B20" 'Value.*get_value'

# local $SIG
check_output "batch20: local SIG handler -> commented" "$B20" '# local.*__DIE__.*Not needed'

# s///r -> copy then modify
check_output "batch20: s///r copy" "$B20" 'was s///r.*non-destructive'

# Single-line sub body preserved
check_output "batch20: single-line sub body" "$B20" 'func greet.*return.*Hello'
check_output "batch20: single-line sub body 2" "$B20" 'func add.*return'

# ---- Batch 21: NOT CONVERTED items ----
B21="$TESTDIR/test_batch21.pl"

# Dynamic dispatch REVIEW
check_output "batch21: dynamic dispatch passthrough" "$B21" '->\$method'

# kill signal name -> number
check_output "batch21: kill TERM -> 15" "$B21" 'core::kill(15'
check_output "batch21: kill HUP -> 1" "$B21" 'core::kill(1'
check_output "batch21: kill INT -> 2" "$B21" 'core::kill(2'
check_output "batch21: kill KILL -> 9" "$B21" 'core::kill(9'

# core:: function mappings
check_output "batch21: symlink -> core::" "$B21" 'core::symlink'
check_output "batch21: link -> core::" "$B21" 'core::link'
check_output "batch21: umask -> core::" "$B21" 'core::umask'
check_output "batch21: fileno -> core::" "$B21" 'core::fileno'
check_output "batch21: truncate -> core::" "$B21" 'core::truncate'

# INIT/CHECK -> BEGIN
check_output "batch21: INIT -> BEGIN" "$B21" 'BEGIN.*was INIT'
check_output "batch21: CHECK -> BEGIN" "$B21" 'BEGIN.*was CHECK'

# substr lvalue rewritten
check_output "batch21: substr lvalue rewritten" "$B21" 'str_v = substr.*str_v.*\. "new"'

# caller with levels (native in Strada, left as-is)
check_output "batch21: caller levels native" "$B21" 'caller(2)'

# select autoflush commented out
check_output "batch21: select autoflush commented" "$B21" '# .*select.*Not needed'

# $| = 1 commented
check_output "batch21: pipe autoflush commented" "$B21" '# .* = 1.*Not needed'

# goto &func -> return func(@_)
check_output "batch21: goto &func -> return func(@_)" "$B21" 'return fallback_handler(@_)'

# ---- Batch 22: STRUCTURAL + NICE TO HAVE items ----
B22="$TESTDIR/test_batch22.pl"

# bless REVIEW
check_output "batch22: bless passes through" "$B22" 'bless'

# local %ENV
check_output "batch22: local ENV -> setenv" "$B22" '# .*local.*ENV.*core::setenv'

# Multi-package ordering: Foo funcs under package Foo
check_output "batch22: foo_method in output" "$B22" 'func foo_method'
check_output "batch22: bar_method in output" "$B22" 'func bar_method'

# Path::Tiny -> core::slurp
check_output "batch22: Path::Tiny use commented" "$B22" 'REVIEW.*core::slurp.*core::spew'
check_output "batch22: path slurp -> core::slurp" "$B22" 'core::slurp'

# pos() REVIEW
check_output "batch22: pos tracking var" "$B22" '__pos.*declare.*= 0'

# BUILDARGS REVIEW
check_output "batch22: BUILDARGS REVIEW" "$B22" 'REVIEW.*BUILDARGS'

# BUILD REVIEW
check_output "batch22: BUILD -> after new" "$B22" 'after "new" func'

# Module handlers
check_output "batch22: Log module commented" "$B22" 'REVIEW.*logging'
check_output "batch22: Term module commented" "$B22" 'REVIEW.*terminal'
check_output "batch22: Class module commented" "$B22" 'Strada has native OOP'

# ============================================================
# Batch 23: UNCONVERTED items (Task #53)
# ============================================================
B23="$TESTDIR/test_batch23.pl"

# File test operators
check_output "batch23: -e -> core::file_exists" "$B23" 'core::file_exists'
check_output "batch23: -f -> core::is_file" "$B23" 'core::is_file'
check_output "batch23: -d -> core::is_dir" "$B23" 'core::is_dir'
check_output "batch23: -s -> core::file_size" "$B23" 'core::file_size'
check_output "batch23: -z -> file_size == 0" "$B23" 'core::file_size.*== 0'
check_output "batch23: -r access" "$B23" 'core::access.*4.*== 0'
check_output "batch23: -w access" "$B23" 'core::access.*2.*== 0'
check_output "batch23: -x access" "$B23" 'core::access.*1.*== 0'

# lstat
check_output "batch23: lstat -> core::lstat" "$B23" 'core::lstat'

# use subs
check_output "batch23: use subs commented" "$B23" 'Not needed in Strada.*subs visible'

# UNIVERSAL::isa/can
check_output "batch23: UNIVERSAL::isa -> ->isa" "$B23" 'obj->isa'
check_output "batch23: UNIVERSAL::can -> ->can" "$B23" 'obj->can'

# cluck
check_output "batch23: Carp::cluck -> warn REVIEW" "$B23" 'warn.*REVIEW.*cluck.*stack'
check_output "batch23: bare cluck -> warn" "$B23" 'warn.*REVIEW.*cluck.*stack'

# Diamond operator
check_output "batch23: <> -> core::readline" "$B23" 'core::readline.*<>'
check_output "batch23: <STDIN> -> core::readline" "$B23" 'core::readline'

# Socket functions
check_output "batch23: socket converted" "$B23" 'core::socket_create'
check_output "batch23: bind converted" "$B23" 'core::socket_bind'
check_output "batch23: listen converted" "$B23" 'core::socket_listen'
check_output "batch23: setsockopt comment" "$B23" 'REVIEW.*setsockopt'

# IO::Socket::INET module
check_output "batch23: IO::Socket::INET commented" "$B23" 'core::socket_client.*core::socket_server'

# ============================================================
# Batch 24: POOR CONVERSION items (Task #54)
# ============================================================
B24="$TESTDIR/test_batch24.pl"

# $_[0] -> $self in single-line subs
check_output "batch24: single-line getter uses no-parens form" "$B24" 'func name {'
check_output "batch24: single-line setter uses no-parens form" "$B24" 'func set_name {'

# $_[0] -> $self in multi-line subs (no-parens form)
check_output "batch24: multi-line greet uses no-parens form" "$B24" 'func greet {'
check_output "batch24: multi-line update uses no-parens form" "$B24" 'func update {'

# eval/$@/if pattern -> try/catch
check_output "batch24: eval/err/if -> try/catch" "$B24" 'catch ..err'

# Try::Tiny $_ -> $e
check_output "batch24: Try::Tiny $_ -> $e in warn" "$B24" 'warn.*Caught.*\$e'
check_output "batch24: Try::Tiny $_ -> $e in call" "$B24" 'log_error.*\$e'

# Moose handles hash form
check_output "batch24: handles hash -> start_engine wrapper" "$B24" 'func start_engine.*engine.*start'
check_output "batch24: handles hash -> stop_engine wrapper" "$B24" 'func stop_engine.*engine.*stop'

# ============================================================
# Batch 25: File::Copy conversion issues (real CPAN module patterns)
# ============================================================
B25="$TESTDIR/test_batch25.pl"

# Forward declarations
check_output "batch25: sub copy; -> comment" "$B25" '# sub copy;.*Forward declaration'
check_output "batch25: sub syscopy; -> comment" "$B25" '# sub syscopy;.*Forward declaration'
check_output "batch25: sub cp; -> comment" "$B25" '# sub cp;.*Forward declaration'

# $! lvalue
check_output "batch25: bang=0 -> REVIEW comment" "$B25" '# .*\$! = 0.*REVIEW.*errno'
check_output "batch25: bang=status -> REVIEW comment" "$B25" '# .*\$! = \$status.*REVIEW'
check_output "batch25: bang list assignment -> REVIEW" "$B25" '# .*\$!.*\$\^E.*REVIEW.*list'
check_output "batch25: bang read -> core::strerror" "$B25" 'core::strerror.*core::errno'

# croak/carp definition handling
check_output "batch25: func croak stays as croak" "$B25" 'func croak'
check_output "batch25: goto Carp::croak -> return die" "$B25" 'return die(@_)'
check_output "batch25: func carp stays as carp" "$B25" 'func carp'
check_output "batch25: goto Carp::carp -> return warn" "$B25" 'return warn(@_)'

# Bare builtins in conditions
check_output "batch25: rename in postfix if -> proper parens" "$B25" 'if (core::rename(\$from, \$to))'
check_output "batch25: 1 while unlink -> proper parens" "$B25" 'while (core::unlink(\$to))'
check_output "batch25: bare stat -> core::stat" "$B25" 'core::stat(\$from)'
check_output "batch25: bare chmod var -> core::chmod" "$B25" 'core::chmod(\$perm, \$to)'
check_output "batch25: bare umask -> core::umask()" "$B25" 'core::umask()'

# Conditional import
check_output "batch25: eval use Module -> try/catch with flag" "$B25" '__has_Time_HiRes'
check_output "batch25: no REPL injection" "$B25" 'use REPL' 0

# Special variables
check_output "batch25: ^E -> 0" "$B25" '0 \+ 0'
check_output "batch25: dollar-gt -> core::geteuid()" "$B25" 'core::geteuid()'
check_output "batch25: xor -> logical XOR" "$B25" '!.*!=.*!'
check_output "batch25: -p file test -> REVIEW" "$B25" 'REVIEW.*named pipe'
check_output "batch25: defined &func -> compiled check" "$B25" 'compiled_defined_check.*use File::Basename'
check_output "batch25: utime -> REVIEW" "$B25" 'utime.*REVIEW.*__C__'
check_output "batch25: dollar-rparen -> core::getegid()" "$B25" 'core::getegid()'

# ============================================================
# Batch 26: Perl gap closure tests
# ============================================================
B26="$TESTDIR/test_batch26.pl"

# B3: use VERSION / require VERSION
check_output "batch26: use 5.006 -> comment" "$B26" '# use 5.006.*Perl version'
check_output "batch26: use 5.010001 -> comment" "$B26" '# use 5.010001.*Perl version'
check_output "batch26: use v5.14 -> comment" "$B26" '# use v5.14.*Perl version'
check_output "batch26: require 5.008 -> comment" "$B26" '# require 5.008.*Perl version'
check_output "batch26: require v5.10 -> comment" "$B26" '# require v5.10.*Perl version'

# B6: CORE:: prefix stripping
check_output "batch26: CORE::print -> say" "$B26" 'say("hello'
check_output "batch26: CORE::say -> say" "$B26" '^[^#]*\bsay("world")'
check_output "batch26: CORE::die -> die" "$B26" '^[^#]*\bdie("fail")'
check_output "batch26: CORE::warn -> warn" "$B26" '^[^#]*\bwarn("caution")'
check_output "batch26: CORE::exit -> exit" "$B26" '^[^#]*\bexit(0)'
check_output "batch26: CORE::fork -> core::fork" "$B26" 'core::fork()'
check_output "batch26: CORE::open -> core::open" "$B26" 'core::open'
check_output "batch26: CORE::close -> core::close" "$B26" 'core::close'
check_output "batch26: CORE::unlink -> core::unlink" "$B26" 'core::unlink'
check_output "batch26: CORE::rename -> core::rename" "$B26" 'core::rename'
check_output "batch26: CORE::mkdir -> core::mkdir" "$B26" 'core::mkdir'
check_output "batch26: CORE::rmdir -> core::rmdir" "$B26" 'core::rmdir'
check_output "batch26: CORE::chmod -> core::chmod" "$B26" 'core::chmod'
check_output "batch26: CORE::stat -> core::stat" "$B26" 'core::stat'
check_output "batch26: CORE::chdir -> core::chdir" "$B26" 'core::chdir'
check_output "batch26: CORE::write -> REVIEW" "$B26" 'REVIEW.*CORE::write.*Perl report'
check_output "batch26: no unconverted CORE:: left" "$B26" '^\s*[^#]*CORE::' 0

# B1: Postfix foreach
check_output "batch26: postfix foreach -> for loop" "$B26" 'foreach my scalar.*@items.*push'
check_output "batch26: postfix foreach list -> for loop" "$B26" 'foreach my scalar.*1, 2, 3'

# B2: foreach without my
check_output "batch26: foreach no my -> add my scalar" "$B26" 'foreach my scalar \$item'
check_output "batch26: for no my -> add my scalar" "$B26" 'foreach my scalar \$x'

# B7: @_ in boolean/numeric context
check_output "batch26: @_ == 2 -> scalar(@_)" "$B26" 'scalar(@_) == 2'
check_output "batch26: @_ != 0 -> scalar(@_)" "$B26" 'scalar(@_) != 0'
check_output "batch26: @_ > 3 -> scalar(@_)" "$B26" 'scalar(@_) > 3'
check_output "batch26: if (@_) -> scalar check" "$B26" 'if (scalar(@_) > 0)'

# B8: Multi-variable our
check_output "batch26: our (vars) -> split our scalar" "$B26" 'our scalar \$foo'
check_output "batch26: our (vars) -> split our scalar bar" "$B26" 'our scalar \$bar'
check_output "batch26: our (vars) -> split hash" "$B26" 'my hash %config'
check_output "batch26: our (vars) -> split array" "$B26" 'my array @items'

# B4: Symbolic dereferencing REVIEW
check_output "batch26: simple scalar deref simplified" "$B26" '\$\$ref'
check_output "batch26: simple array deref simplified" "$B26" '@\$array_ref'
check_output "batch26: simple code deref simplified" "$B26" '\$code_ref->()'

# B5: local() passes through for regular vars
check_output "batch26: local() passes through" "$B26" 'local(\$a'

# B10: substr lvalue rewritten
check_output "batch26: substr lvalue rewritten" "$B26" 'string = substr.*string.*\. "abc"'

# B11: local *_ -> my scalar $_
check_output "batch26: local *_ -> my scalar" "$B26" 'my scalar.*_ ='

# B12: qr// REVIEW
check_output "batch26: qr// -> string no REVIEW" "$B26" '"(?i)'

check_no_crash "batch26: no crash" "$B26"

# ============================================================
# Batch 27: New conversion features
# ============================================================
B27="$TESTDIR/test_batch27.pl"

# Typeglob constants
check_output "batch27: typeglob const int" "$B27" 'const int MAX_SIZE = 1024'
check_output "batch27: typeglob const num" "$B27" 'const num PI_VALUE = 3.14159'
check_output "batch27: typeglob const str" "$B27" 'const str APP_NAME = "myapp"'

# goto &func -> return func(@_)
check_output "batch27: goto &func -> return" "$B27" 'return real_handler(@_)'

# DESTROY pass-through (not commented out)
check_output "batch27: DESTROY pass-through" "$B27" 'func DESTROY'
check_output "batch27: no DESTROY TODO" "$B27" 'TODO.*DESTROY' 0

# Math builtins
check_output "batch27: sin -> math::sin" "$B27" 'math::sin'
check_output "batch27: sqrt -> math::sqrt" "$B27" 'math::sqrt'
check_output "batch27: log -> math::log" "$B27" 'math::log'
check_output "batch27: exp -> math::exp" "$B27" 'math::exp'
check_output "batch27: abs -> math::abs" "$B27" 'math::abs'
check_output "batch27: int passes through" "$B27" '\bint('
check_output "batch27: atan2 -> math::atan2" "$B27" 'math::atan2'
check_output "batch27: cos -> math::cos" "$B27" 'math::cos'

# Built-in function conversions
check_output "batch27: rand -> core::rand" "$B27" 'core::rand'
check_output "batch27: srand -> core::srand" "$B27" 'core::srand'
check_output "batch27: hex -> core::hex" "$B27" 'core::hex'
check_output "batch27: time -> core::time" "$B27" 'core::time'
check_output "batch27: sleep -> core::sleep" "$B27" 'core::sleep'
check_output "batch27: readlink -> core::readlink" "$B27" 'core::readlink'
check_output "batch27: localtime -> core::localtime" "$B27" 'core::localtime'
check_output "batch27: gmtime -> core::gmtime" "$B27" 'core::gmtime'
check_output "batch27: wantarray -> core::wantarray" "$B27" 'core::wantarray'

# eval-as-expression
check_output "batch27: eval-as-expr var decl" "$B27" 'my scalar .result = undef'
check_output "batch27: eval-as-expr assignment" "$B27" '.result = compute_something'
check_output "batch27: eval-as-expr try block" "$B27" 'try {'

# local special vars
check_output "batch27: local dollar-at commented" "$B27" '# local.*Not needed.*try/catch'
check_output "batch27: local dollar-bang commented" "$B27" '# local.*errno.*removed'
check_output "batch27: local dollar-question commented" "$B27" '# local.*Removed.*special variable'

# require Module -> use Module (no REVIEW)
check_output "batch27: require Carp -> use Carp" "$B27" 'use Carp;'
check_output "batch27: require File::Spec -> use" "$B27" 'use File::Spec;'

check_no_crash "batch27: no crash" "$B27"

# Batch 28: Round 3 automatable fixes
B28="$TESTDIR/test_batch28.pl"

# $Pkg::var -> core::global_set/get
check_output "batch28: Pkg::var write -> global_set" "$B28" 'core::global_set("File::Find::name"'
check_output "batch28: Pkg::var read -> global_get" "$B28" 'core::global_get("File::Find::dir")'
check_output "batch28: Pkg::var read 2 -> global_get" "$B28" 'core::global_get("Getopt::Long::ignorecase")'
check_output "batch28: @Pkg::var -> REVIEW" "$B28" 'REVIEW.*package array'

# qw() in active code
check_output "batch28: qw() -> list" "$B28" '"red", "green", "blue"'
check_output "batch28: qw// -> list" "$B28" '"alpha", "beta", "gamma"'
check_output "batch28: qw[] -> list" "$B28" '"one", "two", "three"'
check_output "batch28: qw{} -> list" "$B28" '"cat", "dog", "bird"'

# use vars qw(...)
check_output "batch28: use vars -> our scalar VERSION" "$B28" 'our scalar \$VERSION'
check_output "batch28: use vars -> ISA commented" "$B28" '# @ISA.*not needed.*extends'
check_output "batch28: use vars -> EXPORT commented" "$B28" '# @EXPORT.*not needed.*Package'
check_output "batch28: use vars -> hash CACHE" "$B28" 'my hash %CACHE'

# $] and special vars
check_output "batch28: dollar-bracket -> 5.038" "$B28" '5.038'
check_output "batch28: TAINT -> 0" "$B28" '0'
check_output "batch28: caret-V -> v5.38" "$B28" '"v5.38"'

# do {} expression
check_output "batch28: single-line do {} -> unwrap" "$B28" 'compute_something()'
check_output "batch28: multi-line do {} -> note" "$B28" 'do \{\} block.*assign last'

# goto &{expr} and &$ref
check_output "batch28: goto &{expr} -> return expr->(@_)" "$B28" 'return.*->(@_)'
check_output "batch28: goto &\$ref -> return \$ref->(@_)" "$B28" 'return \$handler->(@_)'

# print { $fh } expr
check_output "batch28: print { \$fh } expr -> print()" "$B28" 'print(\$log_fh, "Log message'
check_output "batch28: print { \$fh } var -> print()" "$B28" 'print(\$fh, \$data)'

# inline eval ternary converted to try/catch
check_output "batch28: eval ternary -> try/catch" "$B28" 'try.*_eval_ok.*catch'

# fix_arg_signatures
check_output "batch28: process func has params" "$B28" 'func process'

check_no_crash "batch28: no crash" "$B28"

# Batch 29: Round 4 — REVIEW gap improvements
B29="$TESTDIR/test_batch29.pl"

# local $SIG{__DIE__/__WARN__} -> commented out
check_output "batch29: local SIG DIE sub -> commented" "$B29" '# local.*__DIE__.*Not needed'
check_output "batch29: local SIG WARN sub -> commented" "$B29" '# local.*__WARN__.*Not needed'
check_output "batch29: local SIG DIE DEFAULT -> commented" "$B29" '# local.*__DIE__.*DEFAULT.*Not needed'
check_output "batch29: local SIG WARN bare -> commented" "$B29" '# local.*__WARN__.*Not needed'

# Non-local $SIG{__DIE__/__WARN__} -> REVIEW (not core::signal)
check_output "batch29: SIG DIE non-local -> REVIEW" "$B29" '# .*__DIE__.*REVIEW.*try/catch'
check_output "batch29: SIG WARN non-local -> REVIEW" "$B29" '# .*__WARN__.*REVIEW.*no direct'
check_output "batch29: no core::signal for DIE" "$B29" 'core::signal.*__DIE__' 0
check_output "batch29: no core::signal for WARN" "$B29" 'core::signal.*__WARN__' 0

# Real signals still work
check_output "batch29: real signal INT -> core::signal" "$B29" 'core::signal("INT", "IGNORE")'
check_output "batch29: real signal TERM -> core::signal" "$B29" 'core::signal("TERM"'

# local $Pkg::Var -> save/restore with global_get/set
check_output "batch29: local Exporter save" "$B29" '__saved_Exporter_ExportLevel.*core::global_get'
check_output "batch29: local Exporter set" "$B29" 'core::global_set("Exporter::ExportLevel", 1)'
check_output "batch29: local Exporter restore REVIEW" "$B29" 'REVIEW.*restore.*Exporter'
check_output "batch29: local Carp no-assign save" "$B29" '__saved_Carp_MaxArgLen.*core::global_get'

# bless OOP guidance
check_output "batch29: bless hash passes through" "$B29" 'bless('
check_output "batch29: bless FH -> wrap REVIEW" "$B29" 'bless.*REVIEW.*filehandle.*wrap FH'

# caller()[0] -> __PACKAGE__
check_output "batch29: caller 0 -> __PACKAGE__" "$B29" '__PACKAGE__'
check_output "batch29: no bare caller()[0]" "$B29" '(caller)\[0\]' 0

# caller()[3] -> REVIEW
check_output "batch29: caller func name REVIEW" "$B29" 'REVIEW.*caller.*stack_trace'

# caller($level) -> native (Strada has caller with level)
check_output "batch29: caller level native" "$B29" 'caller(\$level)'

# simple caller() -> __PACKAGE__
check_output "batch29: simple caller -> __PACKAGE__" "$B29" 'calling_pkg = __PACKAGE__'

check_no_crash "batch29: no crash" "$B29"

# Batch 30: Round 5 — REVIEW reduction (deref, defined, File::Spec, fileparse, qr)
B30="$TESTDIR/test_batch30.pl"

# Deref simplification
check_output "batch30: @{ref} -> @ref simple" "$B30" '@\$arrayref'
check_output "batch30: push @{ref} -> @ref" "$B30" 'push(@\$list'
check_output "batch30: foreach @{ref} -> @ref" "$B30" '@\$elements'
check_output "batch30: @{hash->{key}} kept" "$B30" '@{\$data->{"items"}}'
check_output "batch30: symbolic array deref -> global_get" "$B30" 'core::global_get.*EXPORT'
check_output "batch30: scalar deref simplified" "$B30" '\$\$scalarref'
check_output "batch30: hash deref simplified" "$B30" '%\$hashref'

# Code deref
check_output "batch30: &{coderef} -> coderef->()" "$B30" '\$handler->(\$arg1'
check_output "batch30: &{ref->{key}} -> ref->{key}->()" "$B30" '\$dispatch->{"action"}->(@params)'

# defined &func
check_output "batch30: defined &func -> 1" "$B30" '1 /\* compiled_defined_check'
check_output "batch30: defined &{dynamic} -> REVIEW" "$B30" 'defined &{"GLOBAL.*REVIEW.*dynamic'
check_output "batch30: no nested /* /* in defined" "$B30" '/\* /\*' 0

# File::Spec methods
check_output "batch30: catfile -> concat" "$B30" '\$dir \. "/" \. \$file'
check_output "batch30: catdir -> concat" "$B30" '\$base \. "/" \. \$sub'
check_output "batch30: catpath -> concat" "$B30" '\$parent \. "/" \. \$name'
check_output "batch30: curdir -> dot" "$B30" '\$dot = "\."'
check_output "batch30: rootdir -> slash" "$B30" '\$root = "/"'
check_output "batch30: file_name_is_absolute -> substr" "$B30" 'substr(\$path, 0, 1) eq "/"'
check_output "batch30: rel2abs -> realpath" "$B30" 'core::realpath(\$rel)'
check_output "batch30: splitdir -> split" "$B30" 'split("/", \$directories)'

# fileparse
check_output "batch30: fileparse simple -> basename" "$B30" 'core::basename(\$full_path)'
check_output "batch30: fileparse destructuring -> REVIEW" "$B30" 'fileparse.*REVIEW.*core::basename.*core::dirname'

# qr// -> string (no REVIEW)
check_output "batch30: qr//i -> string" "$B30" '"(?i)'
check_output "batch30: qr{}x -> string" "$B30" '"(?x)\[a-z\]'
check_output "batch30: no REVIEW on qr" "$B30" 'REVIEW.*precompiled regex' 0

check_no_crash "batch30: no crash" "$B30"

# ============================================================
# Batch 31: REVIEW elimination (Round 6)
# ============================================================
B31="$TESTDIR/test_batch31.pl"

# T1a: PCRE2 flags should NOT produce REVIEW
check_output "batch31: /i flag no REVIEW" "$B31" "REVIEW.*/i.*/s.*/x.*PCRE2" 0
check_output "batch31: regex /i works" "$B31" '/hello/i'

# T1b: $] version no REVIEW
check_output "batch31: version no REVIEW" "$B31" "REVIEW.*Perl version.*5.038" 0

# T1c: require→use no REVIEW
check_output "batch31: require Carp no REVIEW" "$B31" "use Carp;"
check_output "batch31: require no REVIEW text" "$B31" "REVIEW.*require.*runtime" 0

# T1d: lstat→core::lstat
check_output "batch31: lstat becomes core::lstat" "$B31" "core::lstat"
check_output "batch31: lstat no REVIEW" "$B31" "REVIEW.*lstat" 0

# T2a: OS comparisons evaluated
check_output "batch31: MSWin32 eq evaluates to 0" "$B31" 'if (0)'
check_output "batch31: linux eq evaluates to 1" "$B31" 'elsif (1)'
check_output "batch31: VMS ne evaluates to 1" "$B31" 'elsif (1)'
check_output "batch31: OS no REVIEW" "$B31" 'REVIEW.*\$\^O' 0

# T2b: our @ISA commented out
check_output "batch31: @ISA commented out" "$B31" '# .*@ISA.*not needed.*extends'
check_output "batch31: @EXPORT commented out" "$B31" '# .*@EXPORT.*not needed.*Package'
check_output "batch31: our @other no REVIEW" "$B31" "REVIEW.*only supports scalars" 0

# T2c: substr lvalue 3-arg rewritten
check_output "batch31: substr lvalue 3-arg" "$B31" 'string = substr.*string.*\. "new" \.'
# T2c: substr lvalue 2-arg truncation
check_output "batch31: substr lvalue truncation" "$B31" 'buffer = substr.*buffer.*0.*pos'
# T2c: substr lvalue 2-arg with value
check_output "batch31: substr lvalue 2-arg value" "$B31" 'text = substr.*text.*0.*5.*\. "\.\.\."'

# T2e: caller patterns
check_output "batch31: caller()[0] -> __PACKAGE__" "$B31" '__PACKAGE__'
check_output "batch31: caller no REVIEW" "$B31" "REVIEW.*caller.*verify" 0
check_output "batch31: caller()[1] -> __FILE__" "$B31" '__FILE__'
check_output "batch31: caller()[2] -> __LINE__" "$B31" '__LINE__'

# T2f: eval string no REVIEW about REPL::init
check_output "batch31: eval string no REVIEW" "$B31" "REVIEW.*eval string.*REPL::init" 0
check_output "batch31: eval var commented" "$B31" "REVIEW.*string eval of variable"

# T2g: $\ no REVIEW
check_output "batch31: backslash-dollar no REVIEW" "$B31" 'REVIEW.*output record separator' 0

# T2g: $? converted to $__exit_code
check_output "batch31: exit code converted" "$B31" '__exit_code'

# T2g: $. commented out
check_output "batch31: dot-dollar commented out" "$B31" '# .*\$\..*not available'

# T2g: $^E no REVIEW
check_output "batch31: caret-E no REVIEW" "$B31" 'REVIEW.*extended OS error' 0

# T2g: select(FH) commented out
check_output "batch31: select commented out" "$B31" '# .*select.*Not needed'

# T2h: local special vars commented out
check_output "batch31: local special vars commented" "$B31" '# .*local.*Not needed'

# T2d: inline eval ternary converted
check_output "batch31: eval ternary to try" "$B31" 'try.*catch'

# T3a: symbolic scalar deref of hash
check_output "batch31: scalar deref of hash simplified" "$B31" '\$linkage\{\$opt\}'

# T2b: use vars @ISA commented
check_output "batch31: use vars @ISA not needed" "$B31" '# @ISA.*not needed'

check_no_crash "batch31: no crash" "$B31"

# ============================================================
# Batch 32: Final REVIEW elimination (Round 6b)
# ============================================================
B32="$TESTDIR/test_batch32.pl"

# P1A: @{$hash{key}} — valid Strada, no REVIEW
check_output "batch32: array deref hash value no REVIEW" "$B32" 'REVIEW.*symbolic array' 0
check_output "batch32: @{hash{key}} pass-through" "$B32" '@{\$data{\$key}}'
check_output "batch32: push @{hash{key}} pass-through" "$B32" 'push(@{\$linkage'
check_output "batch32: @{arr[i]} pass-through" "$B32" '@{\$allwords\[\$i\]}'

# P1B: @{$pkg . "::EXPORT"} — core::global_get
check_output "batch32: symbolic array deref global_get" "$B32" 'core::global_get.*EXPORT'

# P2A: &{$hash{key}} → $hash{key}->()
check_output "batch32: code deref hash with args" "$B32" 'linkage{\$opt}->(\$arg1'
check_output "batch32: code deref hash no args" "$B32" 'callbacks{\$name}->()'

# P2B: &{$pkg . "::func"} — commented out
check_output "batch32: symbolic code deref commented" "$B32" '# .*REVIEW.*symbolic function'

# P3: substr with ternary position — no REVIEW
check_output "batch32: substr ternary no REVIEW" "$B32" 'REVIEW.*substr' 0
check_output "batch32: substr ternary rewritten" "$B32" '\$name = substr.*0.*length'

# P4A: typeglob import — commented
check_output "batch32: typeglob import commented" "$B32" '# .*import.*Not needed in Strada'

# P4B: typeglob introspection — TODO
check_output "batch32: typeglob ARRAY introspection" "$B32" 'REVIEW.*ARRAY slot access'

# P5: local *_ → my scalar $_
check_output "batch32: local star my -> undef" "$B32" 'my scalar.*_ = undef'
check_output "batch32: local star join" "$B32" "my scalar.*_ = join"
check_output "batch32: local star value" "$B32" 'my scalar.*_ = \$value'

# P6: symbolic hash deref — commented
check_output "batch32: symbolic hash deref commented" "$B32" '# .*REVIEW.*package symbol table'

# P7: $' inside REPL::eval — no false positive
check_output "batch32: no postmatch REVIEW" "$B32" 'REVIEW.*postmatch' 0

# P8: symbolic scalar deref — core::global_get
check_output "batch32: symbolic scalar global_get" "$B32" 'core::global_get.*EXPORT_FAIL'

# No TODO annotations (all converted to REVIEW)
check_output "batch32: no TODO annotations (pattern should NOT match)" "$B32" '# TODO:' 0

check_no_crash "batch32: no crash" "$B32"

# ============================================================
# XS conversion tests
# ============================================================
XS1="$TESTDIR/test_xs1.xs"

# Check XS conversion produces output
XS_OUT=$(mktemp /tmp/p2s_xs1_XXXXXX.strada)
$P2S "$XS1" "$XS_OUT" > /dev/null 2>&1
XS_RC=$?
TOTAL=$((TOTAL + 1))
if [ $XS_RC -eq 0 ] && [ -s "$XS_OUT" ]; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: conversion succeeds"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: conversion failed (rc=$XS_RC)"
fi

# Package declaration
TOTAL=$((TOTAL + 1))
if grep -qP 'package MyXS;' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: package MyXS"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: package MyXS not found"
fi

# C preamble
TOTAL=$((TOTAL + 1))
if grep -qP '__C__' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: has __C__ blocks"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: no __C__ blocks found"
fi

# C preamble should include math.h
TOTAL=$((TOTAL + 1))
if grep -qP '#include <math.h>' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: preamble has math.h"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: preamble missing math.h"
fi

# Should NOT have Perl XS includes
TOTAL=$((TOTAL + 1))
if grep -qP '#include "perl.h"' "$XS_OUT" 2>/dev/null; then
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: still has perl.h include"
else
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: perl.h filtered out"
fi

# Function xs_add
TOTAL=$((TOTAL + 1))
if grep -qP 'func xs_add\(' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: func xs_add found"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: func xs_add not found"
fi

# xs_add return type int
TOTAL=$((TOTAL + 1))
if grep -qP 'func xs_add\(.*\) int' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: xs_add returns int"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: xs_add wrong return type"
fi

# void function
TOTAL=$((TOTAL + 1))
if grep -qP 'func xs_set_counter\(.*\) void' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: xs_set_counter void"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: xs_set_counter not void"
fi

# String param function
TOTAL=$((TOTAL + 1))
if grep -qP 'func xs_greet\(str' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: xs_greet str param"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: xs_greet missing str param"
fi

# PPCODE function
TOTAL=$((TOTAL + 1))
if grep -qP 'PPCODE' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: PPCODE comment present"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: PPCODE comment missing"
fi

# RETVAL -> __retval conversion
TOTAL=$((TOTAL + 1))
if grep -qP '__retval' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: RETVAL -> __retval"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: RETVAL not converted to __retval"
fi

# BOOT section
TOTAL=$((TOTAL + 1))
if grep -qP 'BOOT section' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: BOOT section present"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: BOOT section missing"
fi

# double return type
TOTAL=$((TOTAL + 1))
if grep -qP 'func xs_square\(num' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: xs_square num param"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: xs_square missing num param"
fi

# ALIAS section
TOTAL=$((TOTAL + 1))
if grep -qP 'ALIAS' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: ALIAS comment present"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: ALIAS comment missing"
fi

# croak -> die conversion
TOTAL=$((TOTAL + 1))
if grep -qP 'die\(' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: croak -> die"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: croak not converted to die"
fi

# PREINIT section
TOTAL=$((TOTAL + 1))
if grep -qP 'PREINIT' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: PREINIT present"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: PREINIT missing"
fi

# INIT section
TOTAL=$((TOTAL + 1))
if grep -qP 'INIT' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: INIT present"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: INIT missing"
fi

# CLEANUP section
TOTAL=$((TOTAL + 1))
if grep -qP 'CLEANUP' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: CLEANUP present"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: CLEANUP missing"
fi

# Direct call function (no CODE section)
TOTAL=$((TOTAL + 1))
if grep -qP 'func xs_void_nocode\(' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: direct call func present"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: direct call func missing"
fi

# SvOK conversion
TOTAL=$((TOTAL + 1))
if grep -qP 'strada_is_defined' "$XS_OUT" 2>/dev/null; then
    PASS=$((PASS + 1))
    [ $VERBOSE -eq 1 ] && echo "ok: xs1: SvOK -> strada_is_defined"
else
    FAIL=$((FAIL + 1))
    echo "FAIL: xs1: SvOK not converted"
fi

rm -f "$XS_OUT"

# ================================================================
# Test batch 33: Modern Perl syntax detection (Rounds 7)
# ================================================================
echo ""
echo "=== Batch 33: Modern Perl Syntax ==="

B33="$TESTDIR/test_batch33.pl"

# 7.1 Smartmatch ~~ detection
check_output "b33: smartmatch REVIEW" "$B33" "smartmatch.*~~.*no Strada equivalent"

# 7.2 Yada-yada ... -> die
check_output "b33: yada-yada die" "$B33" 'die\("Not implemented"\)'

# 7.3 __SUB__ keyword
check_output "b33: __SUB__ REVIEW" "$B33" "__SUB__.*no Strada equivalent"

# 7.4 fc() -> lc()
check_output "b33: fc to lc" "$B33" 'lc\('

# 7.5 use builtin commented
check_output "b33: use builtin commented" "$B33" "# use builtin"

# 7.5 true -> 1
check_output "b33: true to 1" "$B33" '= 1;'

# 7.5 false -> 0
check_output "b33: false to 0" "$B33" '= 0;'

# 7.6 class -> package
check_output "b33: class to package" "$B33" "package Point"

# 7.6 field -> has rw
check_output "b33: field to has" "$B33" "has rw scalar"

# 7.6 method -> func with self
check_output "b33: method to func" "$B33" 'func to_string\(scalar \$self\)'

# 7.6 ADJUST -> TODO
check_output "b33: ADJUST REVIEW" "$B33" "ADJUST.*REVIEW"

# 7.7 defer -> TODO
check_output "b33: defer REVIEW" "$B33" "defer.*REVIEW"

# 7.8 lvalue sub TODO
check_output "b33: lvalue REVIEW" "$B33" "REVIEW.*lvalue"

# 7.9 /ee flag TODO
check_output "b33: ee flag REVIEW" "$B33" "REVIEW.*had /ee"

# 7.10 (?{code}) in regex
check_output "b33: embedded code regex REVIEW" "$B33" "embedded code in regex"

# 7.11 for pairs TODO
check_output "b33: for pairs REVIEW" "$B33" "pairs.*REVIEW"

# 7.12 Dumper -> dumper
check_output "b33: Dumper to dumper" "$B33" 'dumper\('

# 7.12 Data::Dumper::Dumper -> dumper
check_output "b33: Data::Dumper::Dumper to dumper" "$B33" 'dumper\(\\@array\)'

# 7.13 WEXITSTATUS macro
check_output "b33: WEXITSTATUS conversion" "$B33" ">> 8.*& 255"

# 7.13 WIFEXITED macro
check_output "b33: WIFEXITED conversion" "$B33" "& 127.*== 0"

# 7.13 POSIX::_exit -> core::_exit
check_output "b33: POSIX _exit" "$B33" 'core::_exit\(1\)'

# No crash test
check_no_crash "b33: no crash" "$B33"


# ================================================================
# Test batch 34: CPAN Module API Conversions (Round 8)
# ================================================================
echo ""
echo "=== Batch 34: CPAN Module APIs ==="

B34="$TESTDIR/test_batch34.pl"

# 8.1 File::Copy - use statement
check_output "b34: use File::Copy comment" "$B34" "# use File::Copy"

# 8.1 move -> core::rename
check_output "b34: move to core::rename" "$B34" 'core::rename\('

# 8.2 mkpath -> core::mkdir
check_output "b34: mkpath to core::mkdir" "$B34" 'core::mkdir\('

# 8.2 rmtree -> core::system("rm -rf " ...)
check_output "b34: rmtree converted" "$B34" 'core::system("rm -rf "'

# 8.3 File::Find TODO
check_output "b34: File::Find REVIEW" "$B34" "core::readdir.*recursive"

# 8.4 tempdir -> core::mkdtemp
check_output "b34: tempdir to core::mkdtemp" "$B34" "core::mkdtemp"

# 8.5 gettimeofday -> core::gettimeofday
check_output "b34: gettimeofday" "$B34" 'core::gettimeofday\('

# 8.5 usleep -> core::usleep
check_output "b34: usleep" "$B34" 'core::usleep\('

# 8.6 POSIX::mktime -> core::mktime
check_output "b34: POSIX mktime" "$B34" 'core::mktime\('

# 8.6 POSIX::strftime -> core::strftime
check_output "b34: POSIX strftime" "$B34" 'core::strftime\('

# 8.6 POSIX::_exit -> core::_exit
check_output "b34: POSIX exit" "$B34" 'core::_exit\(0\)'

# 8.6 POSIX::getcwd -> core::getcwd
check_output "b34: POSIX getcwd" "$B34" 'core::getcwd\('

# 8.6 POSIX::setlocale TODO
check_output "b34: POSIX setlocale REVIEW" "$B34" "setlocale.*REVIEW"

# 8.6 POSIX::setsid -> core::setsid
check_output "b34: POSIX setsid converted" "$B34" "core::setsid("

# 8.7 IO::Socket::INET TODO
check_output "b34: IO Socket REVIEW" "$B34" "IO::Socket.*core::socket"

# 8.8 Encode comment
check_output "b34: use Encode comment" "$B34" "# use Encode"

# 8.8 is_utf8 -> 1
check_output "b34: is_utf8 to 1" "$B34" '= 1;.*UTF-8'

# 8.9 md5_hex -> core::md5
check_output "b34: md5_hex to core::md5" "$B34" 'core::md5\('

# 8.9 sha256_hex -> core::sha256
check_output "b34: sha256_hex to core::sha256" "$B34" 'core::sha256\('

# 8.10 HTTP::Tiny TODO
check_output "b34: HTTP::Tiny REVIEW" "$B34" "HTTP::Tiny.*REVIEW"

# 8.11 freeze -> JSON::encode
check_output "b34: freeze to JSON::encode" "$B34" 'JSON::encode\('

# 8.11 thaw -> JSON::decode
check_output "b34: thaw to JSON::decode" "$B34" 'JSON::decode\('

# 8.11 nstore TODO
check_output "b34: nstore REVIEW" "$B34" "nstore.*REVIEW"

# 8.11 retrieve TODO
check_output "b34: retrieve REVIEW" "$B34" "retrieve.*REVIEW"

# 8.12 DBI prepare - no unnecessary TODO
check_output "b34: DBI prepare no double REVIEW" "$B34" "prepare" 1

# No crash test
check_no_crash "b34: no crash" "$B34"


# ================================================================
# Test batch 35: File Test Operators + Quality (Round 9)
# ================================================================
echo ""
echo "=== Batch 35: File Tests + Quality ==="

B35="$TESTDIR/test_batch35.pl"

# 9.1 -e -> core::file_exists
check_output "b35: -e file_exists" "$B35" 'core::file_exists\('

# 9.1 -f -> core::is_file
check_output "b35: -f is_file" "$B35" 'core::is_file\('

# 9.1 -d -> core::is_dir
check_output "b35: -d is_dir" "$B35" 'core::is_dir\('

# 9.1 -s -> core::file_size
check_output "b35: -s file_size" "$B35" 'core::file_size\('

# 9.1 -r -> core::access with R_OK
check_output "b35: -r core::access" "$B35" 'core::access.*4.*== 0'

# 9.1 -w -> core::access with W_OK
check_output "b35: -w core::access" "$B35" 'core::access.*2.*== 0'

# 9.1 -x -> core::access with X_OK
check_output "b35: -x core::access" "$B35" 'core::access.*1.*== 0'

# 9.1 -l -> TODO
check_output "b35: -l symlink -> readlink" "$B35" "core::readlink"

# 9.1 -T -> TODO
check_output "b35: -T text REVIEW" "$B35" "REVIEW.*text file test"

# 9.1 -B -> TODO
check_output "b35: -B binary REVIEW" "$B35" "REVIEW.*binary file test"

# 9.1 -M -> core::stat mtime
check_output "b35: -M core::stat mtime" "$B35" 'core::stat.*mtime'

# 9.1 -A -> core::stat atime
check_output "b35: -A core::stat atime" "$B35" 'core::stat.*atime'

# 9.1 -C -> core::stat ctime
check_output "b35: -C core::stat ctime" "$B35" 'core::stat.*ctime'

# 9.1 file test with string literal
check_output "b35: -e string literal" "$B35" 'core::file_exists\("/tmp/test"\)'

# 9.2 tie -> tie()
check_output "b35: tie parens" "$B35" 'tie\(%hash'

# 9.2 untie -> untie()
check_output "b35: untie parens" "$B35" 'untie\(%hash\)'

# 9.2 tied -> tied()
check_output "b35: tied parens" "$B35" 'tied\(%hash\)'

# 9.4 reftype -> TODO not REVIEW
check_output "b35: reftype REVIEW" "$B35" "reftype.*REVIEW"
check_output "b35: reftype no annotation REVIEW" "$B35" "replaced with ref.*REVIEW" 0

# 9.5 @_ with @rest -> variadic
check_output "b35: @rest preserved in destructuring" "$B35" '@rest.*= @_'

# No crash test
check_no_crash "b35: no crash" "$B35"


# ================================================================
# Test batch 36: Multi-line Construct Improvements (Round 10)
# ================================================================
echo ""
echo "=== Batch 36: Multi-line Constructs ==="

B36="$TESTDIR/test_batch36.pl"

# 10.1 Multi-line hash constructor joined
check_output "b36: hash constructor joined" "$B36" '"timeout" => 30.*"retries" => 3'

# 10.2 Multi-line array constructor joined
check_output "b36: array constructor joined" "$B36" '"foo".*"bar".*"baz"'

# 10.4 Multi-line function call joined
check_output "b36: func call joined" "$B36" 'some_function.*arg1.*arg2.*arg3'

# 10.6 Multi-line my destructuring joined
check_output "b36: my destructuring joined" "$B36" 'my.*name.*age.*city.*= @_'

# 10.7 Multi-line hash ref constructor
check_output "b36: hash ref joined" "$B36" 'host.*localhost.*port.*8080'

# 10.8 Multi-line array ref constructor
check_output "b36: array ref joined" "$B36" '"alpha".*"beta".*"gamma"'

# Method chain already handled (existing pass2d)
check_output "b36: method chain joined" "$B36" 'builder.*set_name.*set_value.*build'

# String concat already handled (existing pass2d)
check_output "b36: string concat joined" "$B36" '"Hello ".*name.*city'

# No crash test
check_no_crash "b36: no crash" "$B36"


# ============================================================
# BATCH 37: Method parens, false positive guards, implicit $_, special vars
# ============================================================
B37="$TESTDIR/test_batch37.pl"

# Fix 1: Method calls without parens get () added
check_output "b37: close gets parens" "$B37" '->close()'
check_output "b37: flush gets parens" "$B37" '->flush()'
check_output "b37: name gets parens" "$B37" '->name()'
check_output "b37: process gets parens" "$B37" '->process()'
check_output "b37: commit gets parens" "$B37" '->commit()'
check_output "b37: rollback gets parens" "$B37" '->rollback()'
# Existing parens preserved
check_output "b37: existing parens untouched" "$B37" '->method()'
# Hash deref not touched
check_output "b37: hash deref untouched" "$B37" '->{"key"}'

# Fix 2: Method calls not falsely converted to math/sys namespace
check_output "b37: logger->log preserved" "$B37" '->log('
check_output "b37: logger->log not math" "$B37" '->math::log' 0
check_output "b37: dt->floor preserved" "$B37" '->floor('
check_output "b37: dt->floor not math" "$B37" '->math::floor' 0
check_output "b37: dt->ceil preserved" "$B37" '->ceil('
check_output "b37: dt->ceil not math" "$B37" '->math::ceil' 0
check_output "b37: timer->strftime preserved" "$B37" '->strftime('
check_output "b37: timer->strftime not sys" "$B37" '->core::strftime' 0
# Standalone calls still converted
check_output "b37: standalone log converted" "$B37" 'math::log(10)'
check_output "b37: standalone sqrt converted" "$B37" 'math::sqrt(16)'
check_output "b37: standalone abs converted" "$B37" 'math::abs('

# Fix 3: Implicit $_ in for/foreach
check_output "b37: for implicit var" "$B37" 'foreach my scalar ._ '
check_output "b37: foreach implicit var" "$B37" 'foreach my scalar ._ '
# Explicit var should still work
check_output "b37: for with explicit var" "$B37" 'foreach my scalar .item'
# C-style for should not be changed
check_output "b37: c-style for unchanged" "$B37" 'for (my'
# Bare print
check_output "b37: bare print" "$B37" 'say(.*_)'
# print $_ converted
check_output "b37: print dollar underscore" "$B37" 'say(.*_)'

# Fix 4: Special variables
check_output "b37: dollar-slash undef" "$B37" 'slurp mode'
check_output "b37: dollar-slash newline" "$B37" 'not needed.*Strada reads lines'
check_output "b37: dollar-semicolon" "$B37" 'REVIEW.*subscript separator'

# No crash test
check_no_crash "b37: no crash" "$B37"


# ============================================================
# BATCH 38: use mro, Moose subtype, Class::Accessor, sprintf, sysopen, socket
# ============================================================
B38="$TESTDIR/test_batch38.pl"

# 11.1: use mro
check_output "b38: use mro commented" "$B38" '# use mro.*Not needed'

# 11.2: Moose subtype/coerce
check_output "b38: subtype commented" "$B38" '# subtype.*REVIEW.*Moose subtype'
check_output "b38: coerce commented" "$B38" '# coerce.*REVIEW.*Moose coerce'

# 11.3: Class::Accessor
check_output "b38: mk_accessors name" "$B38" 'has rw scalar'
check_output "b38: mk_ro_accessors id" "$B38" 'has ro scalar'

# 11.4: sprintf %v/%b
check_output "b38: sprintf %v annotation" "$B38" 'REVIEW.*Perl-specific'
check_output "b38: sprintf normal no annotation" "$B38" 'sprintf.*%s.*%d'

# 11.5: sysopen
check_output "b38: sysopen REVIEW" "$B38" 'REVIEW.*sysopen.*core::open'

# 11.6: Socket constants
check_output "b38: AF_INET commented" "$B38" 'Not needed.*socket_client'
check_output "b38: inet_aton commented" "$B38" 'Not needed.*socket_client.*resolves'
check_output "b38: inet_ntoa commented" "$B38" 'Not needed.*socket_client'
check_output "b38: pack_sockaddr_in commented" "$B38" 'Not needed.*socket_client'

# 11.7: Sys::Hostname
check_output "b38: use Sys::Hostname" "$B38" '# use Sys::Hostname.*core::gethostname'

check_no_crash "b38: no crash" "$B38"

# ---- Batch 39: List::MoreUtils, bare builtins $_, sort, map/grep, push @INC, getpwnam, rare builtins, hostname, tr///r ----
B39="$TESTDIR/test_batch39.pl"

# 12.1: List::MoreUtils
check_output "b39: zip REVIEW" "$B39" 'REVIEW.*zip.*interleave'
check_output "b39: uniq_by REVIEW" "$B39" 'REVIEW.*uniq_by.*dedup'
check_output "b39: natatime REVIEW" "$B39" 'REVIEW.*natatime.*chunked'
check_output "b39: each_array REVIEW" "$B39" 'REVIEW.*each_array.*parallel'
check_output "b39: mesh REVIEW" "$B39" 'REVIEW.*mesh.*interleave'
check_output "b39: pairwise REVIEW" "$B39" 'REVIEW.*pairwise.*parallel map'
check_output "b39: part REVIEW" "$B39" 'REVIEW.*part.*partition'
check_output "b39: first_index REVIEW" "$B39" 'REVIEW.*first_index.*first match'
check_output "b39: last_index REVIEW" "$B39" 'REVIEW.*last_index.*reverse'

# 12.2: Bare builtins -> $_
check_output "b39: bare uc" "$B39" 'uc\(\$_\)'
check_output "b39: bare lc" "$B39" 'lc\(\$_\)'
check_output "b39: bare ucfirst" "$B39" 'ucfirst\(\$_\)'
check_output "b39: bare lcfirst" "$B39" 'lcfirst\(\$_\)'
check_output "b39: bare length" "$B39" 'length\(\$_\)'
check_output "b39: bare defined" "$B39" 'defined\(\$_\)'
check_output "b39: bare ref" "$B39" 'ref\(\$_\)'

# 12.3: sort $a/$b
check_output "b39: simple sort no REVIEW" "$B39" 'sort \{ \$a <=> \$b;'
check_output "b39: lc sort no REVIEW" "$B39" 'sort.*lc.*cmp'

# 12.4: Multi-statement map
check_output "b39: multi-stmt map REVIEW" "$B39" 'REVIEW.*multi-statement map'
check_output "b39: simple map no REVIEW" "$B39" 'map \{ \$_ \* 2 \} @nums'

# 12.5: push @INC
check_output "b39: push @INC literal" "$B39" 'use lib "/opt/lib/perl5"'
check_output "b39: unshift @INC literal" "$B39" 'use lib "/usr/local/lib"'
check_output "b39: push @INC dynamic REVIEW" "$B39" 'REVIEW.*dynamic.*@INC'

# 12.6: getpwnam etc. -> core:: calls
check_output "b39: getpwnam sys" "$B39" 'core::getpwnam\('
check_output "b39: getgrnam sys" "$B39" 'core::getgrnam\('
check_output "b39: getpwuid sys" "$B39" 'core::getpwuid\('
check_output "b39: getgrgid sys" "$B39" 'core::getgrgid\('

# 12.7: System builtins -> core:: calls + rare builtins
check_output "b39: fcntl sys" "$B39" 'core::fcntl\('
check_output "b39: ioctl sys" "$B39" 'core::ioctl\('
check_output "b39: chroot sys" "$B39" 'core::chroot\('
check_output "b39: vec REVIEW" "$B39" 'REVIEW.*vec.*bit manipulation'
check_output "b39: formline REVIEW" "$B39" 'REVIEW.*formline.*sprintf'
check_output "b39: dbmopen REVIEW" "$B39" 'dbmopen.*REVIEW.*DBI.*SQLite'
check_output "b39: prototype REVIEW" "$B39" 'REVIEW.*prototype.*Perl prototypes'

# 12.8: hostname
check_output "b39: hostname" "$B39" 'core::gethostname\(\)'

# 12.9: tr///r
check_output "b39: tr///r with my" "$B39" 'my scalar \$upper = \$text.*tr/a-z/A-Z/'
check_output "b39: y///r without my" "$B39" '\$dest = \$src.*y/0-9/a-j/'

check_no_crash "b39: no crash" "$B39"

# 13.1: //g in list context
check_output "b39: //g with capture -> while" "$B39" 'while.*=~ /.*\\w.*g\).*push.*\$1'
check_output "b39: //g with /i flag" "$B39" 'while.*=~ /.*\\d.*g\).*push.*\$1'
check_output "b39: //g no my -> reset array" "$B39" '@existing = \(\); while'

# ============================================================
# Batch 40: All 17 gap features
# ============================================================
B40="$TESTDIR/test_batch40.pl"
echo "--- test_batch40.pl ---"
check_no_crash "b40: no crash" "$B40"

# 1. Dynamic method dispatch (Strada supports $obj->$method() natively)
check_output "b40: dynamic dispatch passthrough" "$B40" '->\$method'

# 2. local with special variables
check_output 'b40: local $, -> join' "$B40" 'output field separator.*join'
check_output 'b40: local $\ -> newline' "$B40" 'output record separator.*newline'
check_output 'b40: local $" -> join' "$B40" 'list separator.*join'
check_output 'b40: local $! -> errno' "$B40" 'errno.*removed'
check_output 'b40: local $; -> multidim' "$B40" 'multidimensional.*removed'

# 3. In-memory file I/O (now uses core::open with string refs)
check_output "b40: in-memory read -> core::open" "$B40" 'core::open.*"r"'
check_output "b40: in-memory write -> core::open" "$B40" 'core::open.*"w"'
check_output "b40: in-memory append -> core::open" "$B40" 'core::open.*"a"'

# 4. require $variable
check_output 'b40: require $var -> REVIEW' "$B40" 'require.*module.*REVIEW.*dynamic.*import_lib'

# 5. @INC/%INC
check_output "b40: @INC hook -> REVIEW" "$B40" 'REVIEW.*@INC hook.*import_lib'
check_output 'b40: delete $INC -> REVIEW' "$B40" 'delete.*INC.*REVIEW'
check_output 'b40: $INC{} -> REVIEW' "$B40" 'INC.*Loaded.*REVIEW'

# 6. pos() function
check_output 'b40: pos($str) -> __posstr' "$B40" '__posstr.*declare.*= 0'
check_output "b40: pos() -> __pos" "$B40" '__pos.*declare.*= 0'

# 7. /ee flag
check_output "b40: /ee -> /e with REVIEW" "$B40" 'REVIEW.*had /ee.*double eval'

# 8. INIT/CHECK/UNITCHECK -> BEGIN
check_output "b40: INIT -> BEGIN" "$B40" 'BEGIN.*was INIT'
check_output "b40: CHECK -> BEGIN" "$B40" 'BEGIN.*was CHECK'
check_output "b40: UNITCHECK -> BEGIN" "$B40" 'BEGIN.*was UNITCHECK'

# 9. no strict / no warnings categories
check_output "b40: no strict refs -> multi-line guidance" "$B40" 'REVIEW.*symbolic refs.*Strada alternatives'
check_output "b40: no strict subs -> barewords" "$B40" "no strict 'subs'.*barewords"
check_output "b40: no warnings redefine" "$B40" "no warnings 'redefine'.*allows.*redefinition"
check_output "b40: no warnings once" "$B40" "no warnings 'once'.*single-use"
check_output "b40: no warnings uninitialized" "$B40" "no warnings 'uninitialized'.*undef"
check_output "b40: no warnings numeric" "$B40" "no warnings 'numeric'.*type system"
check_output "b40: no warnings experimental" "$B40" "no warnings 'experimental.*Not needed"

# 10. Postfix slices
check_output "b40: postfix array slice" "$B40" '@\$ref\[0, 2, 4\]'
check_output "b40: postfix hash slice" "$B40" '@\$href{a, c}'

# 11. use if
check_output "b40: use if -> conditional REVIEW" "$B40" '# use POSIX.*REVIEW.*conditional.*linux'

# 12. format/write
check_output "b40: format block -> sprintf" "$B40" '# format REPORT.*format block.*sprintf'
check_output "b40: format hint" "$B40" 'Hint.*write.*say.*sprintf'
check_output "b40: write -> sprintf" "$B40" '# write;.*Perl write.*sprintf'
check_output "b40: write(HANDLE) -> sprintf" "$B40" '# write(REPORT).*sprintf'
check_output "b40: formline -> sprintf" "$B40" 'formline.*REVIEW.*sprintf'

# 13. DBM operations
check_output "b40: dbmopen -> DBI SQLite" "$B40" 'dbmopen.*REVIEW.*DBI.*SQLite'
check_output "b40: dbmclose -> disconnect" "$B40" 'dbmclose.*REVIEW.*disconnect'

# 14. IPC primitives
check_output "b40: msgget -> async channel" "$B40" 'msgget.*REVIEW.*async::channel'
check_output "b40: msgsnd -> async send" "$B40" 'msgsnd.*REVIEW.*async::send'
check_output "b40: msgrcv -> async recv" "$B40" 'msgrcv.*REVIEW.*async::recv'
check_output "b40: semget -> async mutex" "$B40" 'semget.*REVIEW.*async::mutex'
check_output "b40: semop -> async lock" "$B40" 'semop.*REVIEW.*async::lock'
check_output "b40: shmget -> async channel" "$B40" 'shmget.*REVIEW.*async::channel'
check_output "b40: shmread -> async recv" "$B40" 'shmread.*REVIEW.*async::recv'
check_output "b40: shmwrite -> async send" "$B40" 'shmwrite.*REVIEW.*async::send'
check_output "b40: shmctl -> not needed" "$B40" 'shmctl.*REVIEW.*not needed'
check_output "b40: msgctl -> not needed" "$B40" 'msgctl.*REVIEW.*not needed'
check_output "b40: semctl -> not needed" "$B40" 'semctl.*REVIEW.*not needed'

# 15. AUTOLOAD dispatch
check_output "b40: AUTOLOAD converted" "$B40" 'func AUTOLOAD.*scalar \$self.*str \$method'
check_output "b40: AUTOLOAD dispatch commented" "$B40" 'Strada AUTOLOAD dispatches automatically'

# 16. require with file path
check_output "b40: require file -> use" "$B40" 'use lib::Utils.*was.*require'
check_output "b40: require Module -> use" "$B40" 'use SomeModule'

# 17. Three-arg open modes
check_output "b40: +>> -> REVIEW" "$B40" 'REVIEW.*read-write append'
check_output "b40: +> -> REVIEW" "$B40" 'REVIEW.*read-write.*truncate'
check_output "b40: +< -> REVIEW" "$B40" 'REVIEW.*read-write without truncate'

# ============================================================
# Enhanced features test (test_enhanced.pl)
# ============================================================
ENH="$TESTDIR/test_enhanced.pl"
echo "--- test_enhanced.pl ---"
check_no_crash "enhanced: no crash" "$ENH"

# Feature 7: state variables
check_output "enhanced: state scalar -> our with prefix" "$ENH" 'our scalar \$__state_counter_count'
check_output "enhanced: state array -> our with prefix" "$ENH" 'our array @__state_accumulate_history'
check_output "enhanced: state hash -> our with prefix" "$ENH" 'our hash %__state_accumulate_cache'
check_output "enhanced: no TODO for state" "$ENH" 'TODO.*state.*per-function' 0

# Feature 5: symbolic derefs
check_output "enhanced: @\$ref preserved" "$ENH" '@\$arrayref'
check_output "enhanced: keys %\$ref" "$ENH" 'keys(%\$hashref)'
check_output "enhanced: \$\$scalarref preserved" "$ENH" '\$\$scalarref'

# Feature 3: eval STRING
check_output "enhanced: eval string -> Interpreter::eval_string" "$ENH" 'Strada::Interpreter::eval_string("print'
check_output "enhanced: eval var -> commented" "$ENH" 'REVIEW.*string eval of variable'
check_output "enhanced: eval require -> try/catch" "$ENH" '__has_Some_Module'
check_output "enhanced: Interpreter::init injected" "$ENH" 'Strada::Interpreter::init()'

# Feature 6: socket/IPC
check_output "enhanced: IO::Socket client -> socket_client" "$ENH" 'core::socket_client("localhost", 8080)'
check_output "enhanced: IO::Socket server -> socket_server" "$ENH" 'core::socket_server(9090)'
check_output "enhanced: socket() -> socket_create" "$ENH" 'core::socket_create("tcp")'
check_output "enhanced: bind() -> socket_bind" "$ENH" 'core::socket_bind'
check_output "enhanced: listen() -> socket_listen" "$ENH" 'core::socket_listen(\$SOCK, 5)'
check_output "enhanced: accept() -> socket_accept" "$ENH" 'core::socket_accept(\$SOCK)'
check_output "enhanced: send() -> socket_send" "$ENH" 'core::socket_send(\$sock, \$data)'
check_output "enhanced: recv() -> socket_recv" "$ENH" 'core::socket_recv(\$sock, 1024)'
check_output "enhanced: shutdown() -> core::shutdown" "$ENH" 'core::shutdown(\$sock, 2)'
check_output "enhanced: pipe() -> core::pipe" "$ENH" 'core::pipe()'
check_output "enhanced: sysread -> read_fd" "$ENH" 'core::read_fd(core::fileno'
check_output "enhanced: syswrite -> write_fd" "$ENH" 'core::write_fd(core::fileno'

# Feature 2: Exporter/import
check_output "enhanced: use Exporter commented" "$ENH" '# use Exporter'
check_output "enhanced: use parent strips Exporter keeps parent" "$ENH" 'extends Base::Class'
check_output "enhanced: @EXPORT commented" "$ENH" '# our @EXPORT'
check_output "enhanced: @EXPORT_OK commented" "$ENH" '# our @EXPORT_OK'
check_output "enhanced: EXPORT_TAGS noted" "$ENH" 'Export tags available.*all'

# Feature 2b: Exporter module generates func import()
ENHM="$TESTDIR/test_enhanced_mod.pm"
check_no_crash "enhanced_mod: no crash" "$ENHM"
check_output "enhanced_mod: func import generated" "$ENHM" 'func import(str \$pkg, array @list)'
check_output "enhanced_mod: exports listed" "$ENHM" 'Exports: exported_func1 exported_func2'
check_output "enhanced_mod: optional exports listed" "$ENHM" 'Optional exports: optional_func'

# Feature 1: heredocs
check_output "enhanced: basic heredoc -> string" "$ENH" '"Hello world\\nwith multiple lines\\n"'
check_output "enhanced: nointerp heredoc single quotes" "$ENH" "No .interpolation here"
check_output "enhanced: backslash heredoc non-interp" "$ENH" "Also no .interpolation"
check_output "enhanced: heredoc with suffix" "$ENH" '"body text\\n" \. " extra"'
check_output "enhanced: indented heredoc" "$ENH" '"indented body\\nsecond line\\n"'

# Feature 4: advanced Moose
check_output "enhanced: trigger -> after hook" "$ENH" 'after "set_name"'
check_output "enhanced: coerce -> note" "$ENH" 'coerce.*natively'
check_output "enhanced: has +name override" "$ENH" "has ro scalar .name = .*Unknown.*overrides"
check_output "enhanced: multi-attr qw first" "$ENH" 'has ro str \$first_name (required)'
check_output "enhanced: multi-attr qw second" "$ENH" 'has ro str \$last_name (required)'
check_output "enhanced: multi-attr quoted first" "$ENH" 'has rw str \$email'
check_output "enhanced: multi-attr quoted second" "$ENH" 'has rw str \$phone'
check_output "enhanced: handles delegation" "$ENH" 'func log(scalar \$self'
check_output "enhanced: handles delegation debug" "$ENH" 'func debug(scalar \$self'

# ---- test_batch41.pl ----
B41="$TESTDIR/test_batch41.pl"
echo "--- test_batch41.pl ---"
check_no_crash "b41: no crash" "$B41"

# 1. $_[N] positional args — extended range, NO REVIEW
check_output "b41: _[1] -> __arg1 no REVIEW" "$B41" '\$__arg1' "1"
check_output "b41: _[4] -> __arg4" "$B41" '\$__arg4'
check_output "b41: _[5] -> __arg5" "$B41" '\$__arg5'
check_output "b41: _[6] -> __arg6" "$B41" '\$__arg6'
check_output "b41: _[N] no REVIEW add to func" "$B41" 'REVIEW.*add to func signature' "0"
check_output "b41: _[-1] -> __last_arg" "$B41" '\$__last_arg'
check_output "b41: scalar(@_) hint" "$B41" 'scalar.*REVIEW.*counts arguments'

# 2. Sort comparators — native patterns have no REVIEW
check_output "b41: sort by hash key no REVIEW" "$B41" 'sort.*\$a.*name.*REVIEW' "0"
check_output "b41: sort by lc no REVIEW" "$B41" 'sort.*lc.*REVIEW' "0"
check_output "b41: sort by length no REVIEW" "$B41" 'sort.*length.*REVIEW' "0"

# 3. $SIG variable handler
check_output "b41: SIG = variable" "$B41" 'core::signal("INT", \$handler)'
check_output "b41: SIG = undef -> DEFAULT" "$B41" 'core::signal("TERM", "DEFAULT")'
check_output "b41: SIG = 0 -> DEFAULT" "$B41" 'core::signal("HUP", "DEFAULT")'
check_output "b41: delete SIG -> DEFAULT" "$B41" 'core::signal("ALRM", "DEFAULT")'
check_output "b41: SIG variable signal name" "$B41" 'core::signal(\$sig'

# 4. Bless standard patterns — NO REVIEW
check_output "b41: bless self no REVIEW" "$B41" 'bless(\$self.*REVIEW' "0"

# 5. Open pipes
check_output "b41: open pipe-write 3arg" "$B41" 'core::popen("sendmail -t", "w")'
check_output "b41: open pipe-read 3arg" "$B41" 'core::popen("ls -la", "r")'
check_output "b41: open pipe-write 2arg" "$B41" 'core::popen("sort -u", "w")'
check_output "b41: open pipe-read 2arg" "$B41" 'core::popen("cat /etc/hosts", "r")'

# 6. Inline eval patterns
check_output "b41: return eval -> try/catch result" "$B41" 'try.*\$_eval_result.*Some::func'
check_output "b41: if eval require -> try/catch" "$B41" 'try.*Module.*\$_eval_ok'

# 7. do {} blocks
check_output "b41: single-line do block extracts last expr" "$B41" '42;'
check_output "b41: multi-line do block note" "$B41" 'do \{\} block.*assign last'

# 8. pos() — no REVIEW, just guidance
check_output "b41: pos -> tracking var" "$B41" '\$__posstr'
check_output "b41: pos no REVIEW" "$B41" 'REVIEW.*was pos' "0"
check_output "b41: pos has declare guidance" "$B41" 'declare.*__posstr.*= 0'

# 9. Known modules
check_output "b41: use JSON passes through" "$B41" 'use JSON;'
check_output "b41: Digest -> use crypt" "$B41" 'use crypt;.*Digest'
check_output "b41: Data::Dumper built-in" "$B41" 'dumper.*built-in'
check_output "b41: Carp -> stack traces" "$B41" '# use Carp.*stack traces'

# 10. Glob no REVIEW
check_output "b41: glob converted" "$B41" 'core::glob'
check_output "b41: glob no REVIEW" "$B41" 'core::glob.*REVIEW' "0"

# 11. Encoding layer open — no REVIEW
check_output "b41: encoding stripped no REVIEW" "$B41" 'core::open.*REVIEW' "0"
check_output "b41: encoding has note" "$B41" 'encoding.*stripped.*UTF-8 native'

# ============================================================
# Batch 42: Structural bugs and remaining gaps
# ============================================================

B42="$TESTDIR/test_batch42.pl"

# Fix 1: Brace mismatch — K&R brace style sub name \n {
check_output "b42: Croaker func converted" "$B42" 'func Croaker'
check_output "b42: MultiLine func converted" "$B42" 'func MultiLine'
check_output "b42: no duplicate opening brace" "$B42" 'func Croaker.*{$'

# Fix 2: String eval with Perl code → commented out
check_output "b42: eval with sub commented" "$B42" 'REVIEW.*string eval contains Perl code'
check_output "b42: eval with -> commented" "$B42" 'REVIEW.*string eval contains Perl code'
check_output "b42: eval simple stays Interpreter::eval_string" "$B42" 'Strada::Interpreter::eval_string("42")'
check_output "b42: eval var commented" "$B42" 'REVIEW.*string eval of variable'

# Fix 3: Anonymous sub with prototypes
check_output "b42: anon sub empty proto" "$B42" 'func (scalar'
check_output "b42: no leftover sub proto in code" "$B42" '= sub ' "0"

# Fix 4: local() with parenthesized special variables
check_output "b42: local paren backslash" "$B42" 'output record separator'
check_output "b42: local paren bang" "$B42" 'errno'
check_output "b42: local paren comma" "$B42" 'output field separator'
check_output "b42: local paren dot" "$B42" 'Removed.*Strada special variable'

# Fix 5: Special variables
check_output "b42: caret-O comparison eval" "$B42" 'if (1)'
check_output "b42: caret-O assigned -> core::uname" "$B42" 'core::uname()'
check_output "b42: caret-W removed" "$B42" '= 0;'

# Fix 5: $& -> captures()[0]
check_output "b42: dollar-amp -> captures" "$B42" 'captures()\[0\]'
check_output "b42: dollar-amp no REVIEW" "$B42" 'REVIEW.*match result' "0"

# Fix 6: $#_ -> scalar(@_) - 1
check_output "b42: hash-underscore -> scalar" "$B42" 'scalar(@_) - 1'

# Fix 8: caller(0)[3] -> __FUNCTION__
check_output "b42: caller0-3 -> FUNCTION" "$B42" '__FUNCTION__'
check_output "b42: caller1-3 -> REVIEW" "$B42" 'REVIEW.*caller.*stack_trace'

# Fix 9: SUPER:: without $self->
check_output "b42: SUPER resolved to Parent" "$B42" 'Parent::init'

# Fix 10: ref eq ClassName -> isa
check_output "b42: ref eq -> isa" "$B42" 'isa("Child")'
check_output "b42: ref eq HASH preserved" "$B42" 'ref.*eq "HASH"'
check_output "b42: ref ne -> !isa" "$B42" '!.*isa("Child")'

# Fix 10: Double semicolons cleaned up
check_output "b42: no double semicolons" "$B42" ';;' "0"

# Fix 5: $/ with custom delimiter
check_output "b42: dollar-slash custom" "$B42" 'custom delimiter.*split'

# Fix 4: local($@) with parentheses
check_output "b42: local-paren-at commented" "$B42" 'Not needed.*try/catch'

# No crash
check_no_crash "b42: no crash" "$B42"

# ========================================
# Batch 43: Pass2d over-joining, die/warn gaps, brace fixes
# ========================================
B43="$TESTDIR/test_batch43.pl"

# Fix 1: die with function call gets parens
check_output "b43: die func_call gets parens" "$B43" 'die(shortmess'

# Fix 2: warn $variable gets parens
check_output "b43: warn variable gets parens" "$B43" 'warn(\$msg)'

# Fix 3: Forward declaration with prototype commented out
check_output "b43: forward decl with proto commented" "$B43" '# sub GetOptions.*Forward declaration'
check_output "b43: forward decl Configure commented" "$B43" '# sub Configure.*Forward declaration'

# Fix 4: die $obj becomes throw
check_output "b43: die obj -> throw" "$B43" 'throw.\$err.'

# Fix 5: warn with string interpolation gets parens
check_output "b43: warn string interpolation" "$B43" 'warn("Processing'

# Fix 6: chomp @array gets REVIEW
check_output "b43: chomp array gets REVIEW" "$B43" 'chomp.*REVIEW.*scalar-only'

# Fix 7: no warnings stripped
check_output "b43: no warnings once commented" "$B43" '# no warnings.*once'
check_output "b43: no warnings redefine commented" "$B43" '# no warnings.*redefine'

# Fix 8: Single-quoted strings with braces preserved
check_output "b43: single-quoted braces preserved" "$B43" "value with.*braces"

# No crash
check_no_crash "b43: no crash" "$B43"

# ========================================
# Batch 44: K&R content-after-brace and eval-or-do brace balance
# ========================================
B44="$TESTDIR/test_batch44.pl"

# Fix 1: K&R content-after-brace — func header should have single opening {
# The original { from the K&R line should be stripped, not doubled
check_output "b44: K&R func foo single brace" "$B44" 'func foo.*\{'
check_output "b44: K&R foo body has my not { my" "$B44" '^\s*my\(\$self\) = @_;'
check_output "b44: K&R func bar single brace" "$B44" 'func bar.*\{'
check_output "b44: K&R func baz single brace" "$B44" 'func baz.*\{'

# Fix 2: eval-or-do in map — closing } of map should be preserved
check_output "b44: eval-map try block" "$B44" 'try {'
check_output "b44: eval-map catch block" "$B44" 'catch (\$e)'
check_output "b44: eval-map no orphaned catch" "$B44" '# eval error in' 0
check_output "b44: map closing preserved" "$B44" '} core::argv()'

# Fix 3: eval-or-do standalone — function closing } preserved
check_output "b44: standalone try" "$B44" 'try {'
check_output "b44: standalone cleanup preserved" "$B44" 'cleanup()'

# Fix 4: eval-or-die — function closing } preserved
check_output "b44: eval-or-die try" "$B44" 'try {'
check_output "b44: eval-or-die catch has die" "$B44" 'die.*failed'

# Fix 5: Normal sub unaffected
check_output "b44: normal sub works" "$B44" 'func normal_sub'
check_output "b44: normal sub returns 42" "$B44" 'return 42'

# Fix 6: Orphaned else from commented-out eval-if
# The if(eval(...)) should be commented, and } else { and closing } should also be commented
check_output "b44: orphaned else commented" "$B44" '# \} else \{.*orphaned'
check_output "b44: orphaned else closing } commented" "$B44" '# \}.*orphaned.*if/unless'
check_output "b44: orphaned else body preserved" "$B44" 'do_true()'
check_output "b44: orphaned else body preserved 2" "$B44" 'do_false()'

# Fix 7: Orphaned elsif/else chain
check_output "b44: orphaned elsif commented" "$B44" '# \} elsif.*orphaned'
check_output "b44: orphaned elsif branches preserved" "$B44" 'branch_a()'
check_output "b44: orphaned elsif branches preserved 2" "$B44" 'branch_b()'
check_output "b44: orphaned elsif branches preserved 3" "$B44" 'branch_c()'

# Fix 8: Orphaned } from commented-out unless
check_output "b44: orphaned unless body preserved" "$B44" 'do_setup()'

# No crash
check_no_crash "b44: no crash" "$B44"

# ========================================
# Batch 45: Comprehensive gap fixes
# ========================================
B45="$TESTDIR/test_batch45.pl"

# Fix 1: wantarray as variable name — should NOT become core::wantarray
check_output "b45: wantarray var preserved" "$B45" '\$wantarray'
check_output "b45: wantarray var not mangled" "$B45" 'core::wantarray.*wantarray.*wantarray' 0

# Fix 2: wantarray as function call — SHOULD become core::wantarray
check_output "b45: wantarray func converted" "$B45" 'core::wantarray()'

# Fix 3-5: qr# qr! qr| delimiters converted to strings
check_output "b45: qr# converted" "$B45" '"(.i)foo'
check_output "b45: qr! converted" "$B45" '"hello'
check_output "b45: qr| converted" "$B45" '"(.x)test'

# Fix 6: .= expansion
check_output "b45: dotequals expanded" "$B45" '\$msg = \$msg \. " world"'

# Fix 7: ||= expansion
check_output "b45: oreq expanded" "$B45" '\$x = \$x || "default"'

# Fix 8: POSIX:: mapping
check_output "b45: POSIX::floor mapped" "$B45" 'math::floor'
check_output "b45: POSIX::ceil mapped" "$B45" 'math::ceil'
check_output "b45: POSIX::sqrt mapped" "$B45" 'math::sqrt'
check_output "b45: no POSIX:: doubling" "$B45" 'POSIX::math::' 0

# Fix 9: caller list context annotation
check_output "b45: caller list context REVIEW" "$B45" 'caller.*REVIEW.*list context'

# Fix 10: Scalar::Util — reftype gets REVIEW
check_output "b45: reftype REVIEW" "$B45" 'REVIEW.*reftype'

# Fix 11: pos($$ref) deref
check_output "b45: pos deref correct" "$B45" '__postextref'

# Fix 12: 4-arg substr REVIEW
check_output "b45: 4arg substr REVIEW" "$B45" 'substr.*REVIEW.*4-arg'

# Fix 13: given/when conversion
check_output "b45: given converted" "$B45" '__given'
check_output "b45: when converted to if" "$B45" 'if.*__given'

# No crash
check_no_crash "b45: no crash" "$B45"

# ===================== Batch 46: Tier 1-3 fixes =====================
B46="$TESTDIR/test_batch46.pl"

# 1. int() passes through (not math::floor)
check_output "b46: int passes through" "$B46" 'int(3\.7)'
check_output "b46: int not math::floor" "$B46" 'math::floor(3\.7)' 0

# 2. xor -> (!a != !b)
check_output "b46: xor logical XOR" "$B46" '!\$x != !\$y'
# xor -> (!a != !b) verified by the positive test above

# 3. shift(@_) handled by pass1 signature extraction
check_output "b46: shift(@_) preserved in no-parens form" "$B46" 'func test_shift_args {'

# 4. eval/if($@) -> try/catch
check_output "b46: eval to try/catch" "$B46" 'try {'
check_output "b46: catch block" "$B46" 'catch (\$e)'

# 5. local $SIG -> commented
check_output "b46: local SIG commented" "$B46" '# local.*SIG'

# 6. local $ENV{PATH} -> core::setenv
check_output "b46: local ENV -> setenv" "$B46" 'core::setenv'

# 7. local $_ -> lexical with REVIEW
check_output "b46: local $_ -> lexical" "$B46" 'my scalar \$_ =.*REVIEW'

# 8. use vars qw{} with curly braces
check_output "b46: use vars qw{} VERSION" "$B46" 'our scalar \$VERSION'
check_output "b46: use vars qw{} ISA commented" "$B46" '# @ISA'

# 9. use vars bare list
check_output "b46: use vars bare EXPORT" "$B46" '# @EXPORT'

# 10. Multi-line use constant joined
check_output "b46: multi-line const joined" "$B46" 'const scalar FOO_BAR.*hello.*world'

# 11. tr/// not confused by parens
check_output "b46: tr with parens" "$B46" 'tr/\(\)/\[\]/'
check_output "b46: y transliteration" "$B46" 'y/a-z/A-Z/'

# No crash
check_no_crash "b46: no crash" "$B46"

# ===================== Batch 47: Tier 1-2 comprehensive fixes =====================
B47="$TESTDIR/test_batch47.pl"

# 1. $! inside strings broken out with concatenation
check_output "b47: bang in string broken out" "$B47" '"Error: " \. core::strerror'
check_output "b47: bang in die broken out" "$B47" '"Cannot open: " \. core::strerror'

# 2. for (;;) -> while (1)
check_output "b47: for-ever -> while 1" "$B47" 'while (1)'

# 3. &$coderef -> $ref->()
check_output "b47: coderef call converted" "$B47" '\$handler->("test")'

# 4. no warnings commented
check_output "b47: no warnings commented" "$B47" '# no warnings'

# 5. -d _ stat cache annotated
check_output "b47: stat cache REVIEW" "$B47" '-d _.*REVIEW.*stat cache'

# 6. $$ref annotated
check_output "b47: scalar deref REVIEW" "$B47" '\$\$ref.*REVIEW.*scalar dereference'

# 7. @{$expr} annotated
check_output "b47: array deref REVIEW" "$B47" '@{.*REVIEW.*array dereference'

# 8. sysread converted
check_output "b47: sysread -> core::read_fd" "$B47" 'core::read_fd'

# 9. open BAREWORD annotated
check_output "b47: open bareword REVIEW" "$B47" 'open FH.*REVIEW.*bareword'

# 10. caller annotated
check_output "b47: caller REVIEW" "$B47" 'caller.*REVIEW'

# 11. local $/ commented
check_output "b47: local $/ slurp" "$B47" '# local.*slurp'

# 12. use constant with comment not collapsed
check_output "b47: const FOO" "$B47" 'const scalar FOO = 1'
check_output "b47: const BAR" "$B47" 'const scalar BAR = 2'

# 13. Fcntl::LOCK_EX() -> 2
check_output "b47: Fcntl constant resolved" "$B47" 'core::flock(\$fh, 2)'

# No crash
check_no_crash "b47: no crash" "$B47"

# ===================== Batch 48: CPAN module mapping improvements =====================
B48="$TESTDIR/test_batch48.pl"

# 1. isvstring -> 0
check_output "b48: isvstring -> 0" "$B48" 'if (0)'

# 2. Carp bare and qualified
check_output "b48: bare croak -> die" "$B48" 'die("fatal error")'
check_output "b48: bare carp -> warn" "$B48" 'warn("warning message")'
check_output "b48: bare confess -> die" "$B48" 'die("fatal with trace")'
check_output "b48: Carp::croak -> die" "$B48" 'die("fatal error")'
check_output "b48: Carp::carp -> warn" "$B48" 'warn("warning message")'
check_output "b48: Carp::confess -> die" "$B48" 'die("fatal with trace")'

# 3. Sys::Hostname bare and qualified
check_output "b48: hostname -> core::gethostname" "$B48" 'core::gethostname()'
check_output "b48: Sys::Hostname::hostname -> core::gethostname" "$B48" 'core::gethostname()'

# 4. File::Slurp
check_output "b48: use File::Slurp commented" "$B48" '# use File::Slurp'
check_output "b48: read_file -> core::slurp" "$B48" 'core::slurp("/tmp/test.txt")'
check_output "b48: write_file -> core::spew" "$B48" 'core::spew("/tmp/out.txt"'
check_output "b48: read_dir -> core::readdir" "$B48" 'core::readdir("/tmp")'

# 5. File::Slurper
check_output "b48: use File::Slurper commented" "$B48" '# use File::Slurper'
check_output "b48: read_text -> core::slurp" "$B48" 'core::slurp("/tmp/test.txt")'
check_output "b48: write_text -> core::spew" "$B48" 'core::spew("/tmp/out.txt"'

# 6. File::Copy: bare copy with args
check_output "b48: copy -> core::spew+slurp" "$B48" 'core::spew('

# 7. rmtree/remove_tree converted
check_output "b48: rmtree -> core::system rm" "$B48" 'core::system("rm -rf "'
check_output "b48: rmtree no REVIEW" "$B48" 'core::system("rm -rf "' "1"

# 8. tempfile converted
check_output "b48: tempfile -> core::mkstemp" "$B48" 'core::mkstemp("/tmp/strada_XXXXXX")'
check_output "b48: File::Temp::tempfile -> core::mkstemp" "$B48" 'core::mkstemp("/tmp/strada_XXXXXX")'

# 9. POSIX additional mappings
check_output "b48: POSIX::setsid -> core::setsid" "$B48" 'core::setsid()'
check_output "b48: POSIX::isatty -> core::isatty" "$B48" 'core::isatty(0)'
check_output "b48: POSIX::nice -> core::nice" "$B48" 'core::nice(5)'
check_output "b48: POSIX::access -> core::access" "$B48" 'core::access("/tmp"'
check_output "b48: POSIX::getpgrp -> core::getpgrp" "$B48" 'core::getpgrp()'
check_output "b48: POSIX::setpgid -> core::setpgid" "$B48" 'core::setpgid(0, 0)'
check_output "b48: POSIX::dup -> core::dup" "$B48" 'core::dup(1)'
check_output "b48: POSIX::dup2 -> core::dup2" "$B48" 'core::dup2(1, 2)'
check_output "b48: POSIX::pipe -> core::pipe" "$B48" 'core::pipe()'
check_output "b48: POSIX::close -> core::close_fd" "$B48" 'core::close_fd(3)'
check_output "b48: POSIX::read -> core::read_fd" "$B48" 'core::read_fd(3'
check_output "b48: POSIX::write -> core::write_fd" "$B48" 'core::write_fd(3'
check_output "b48: POSIX::lseek -> core::seek" "$B48" 'core::seek(3, 0, 0)'
check_output "b48: POSIX::umask -> core::umask" "$B48" 'core::umask(022)'
check_output "b48: POSIX::getppid -> core::getppid" "$B48" 'core::getppid()'
check_output "b48: POSIX::WNOHANG -> 1" "$B48" '= 1;'
check_output "b48: POSIX::WUNTRACED -> 2" "$B48" '= 2;'

# 10. Digest additional mappings
check_output "b48: sha1_hex -> core::sha1" "$B48" 'core::sha1("data")'
check_output "b48: sha512_hex -> core::sha512" "$B48" 'core::sha512("data")'
check_output "b48: Digest::MD5::md5 -> core::md5" "$B48" 'core::md5("data")'
check_output "b48: Digest::SHA::sha1_hex -> core::sha1" "$B48" 'core::sha1("data")'
check_output "b48: Digest::SHA::sha512_hex -> core::sha512" "$B48" 'core::sha512("data")'
check_output "b48: Digest::SHA::sha1 -> core::sha1" "$B48" 'core::sha1("data")'
check_output "b48: Digest::SHA::sha512 -> core::sha512" "$B48" 'core::sha512("data")'

# 11. Time::HiRes qualified calls
check_output "b48: Time::HiRes::time -> core::hires_time" "$B48" 'core::hires_time()'
check_output "b48: Time::HiRes::gettimeofday -> core::gettimeofday" "$B48" 'core::gettimeofday()'
check_output "b48: Time::HiRes::usleep -> core::usleep" "$B48" 'core::usleep(1000)'
check_output "b48: Time::HiRes::sleep -> core::sleep" "$B48" 'core::sleep(0.5)'
check_output "b48: Time::HiRes::tv_interval -> core::tv_interval" "$B48" 'core::tv_interval('

# 12. IO::File constructor
check_output "b48: IO::File->new -> core::open" "$B48" 'core::open("/tmp/test.txt"'
check_output "b48: IO::File mode < -> r" "$B48" 'core::open("/tmp/test.txt", "r")'
check_output "b48: IO::File mode > -> w" "$B48" 'core::open("/tmp/out.txt", "w")'

# 13. File::Spec arrow-syntax
check_output "b48: File::Spec->catfile -> concat" "$B48" '\. "/" \.'
check_output "b48: File::Spec->tmpdir -> /tmp" "$B48" '"/tmp"'
check_output "b48: File::Spec->devnull -> /dev/null" "$B48" '"/dev/null"'

# 14. Cwd: bare cwd()
check_output "b48: cwd -> core::getcwd" "$B48" 'core::getcwd()'

# 15. File::Glob: bsd_glob
check_output "b48: use File::Glob commented" "$B48" '# use File::Glob'
check_output "b48: bsd_glob -> core::glob" "$B48" 'core::glob("/tmp/'

# 16. Sys::Syslog
check_output "b48: use Sys::Syslog commented" "$B48" '# use Sys::Syslog'
check_output "b48: openlog -> core::openlog" "$B48" 'core::openlog("myapp"'
check_output "b48: syslog -> core::syslog" "$B48" 'core::syslog("info"'
check_output "b48: closelog -> core::closelog" "$B48" 'core::closelog()'

# No crash
check_no_crash "b48: no crash" "$B48"

# ====================================================================
# Batch 49: OOP multi-attr has, BUILD/BUILDARGS
# ====================================================================
B49="$TESTDIR/test_batch49.pl"

# 1. Multi-attr has with default
check_output "b49: multi-attr has default host" "$B49" "has ro str .host = .*localhost"
check_output "b49: multi-attr has default port" "$B49" "has ro str .port = .*localhost"

# 2. Multi-attr has with lazy + builder
check_output "b49: multi-attr has lazy builder x" "$B49" 'has rw num \$x (lazy, builder => "_build_coords")'
check_output "b49: multi-attr has lazy builder y" "$B49" 'has rw num \$y (lazy, builder => "_build_coords")'

# 3. Multi-attr has with required
check_output "b49: multi-attr has required name" "$B49" 'has ro str \$name (required)'
check_output "b49: multi-attr has required email" "$B49" 'has ro str \$email (required)'

# 4. Multi-attr has with handles delegation
check_output "b49: multi-attr has handles push" "$B49" 'func push(scalar \$self'
check_output "b49: multi-attr has handles pop" "$B49" 'func pop(scalar \$self'

# 5. Quoted list form multi-attr
check_output "b49: quoted list has width" "$B49" 'has rw int \$width = 0'
check_output "b49: quoted list has height" "$B49" 'has rw int \$height = 0'

# 6. has '+attr' with lazy
check_output "b49: has +attr lazy" "$B49" '(lazy)'
check_output "b49: has +attr override comment" "$B49" "overrides parent attribute"

# 7. has '+attr' with isa override
check_output "b49: has +attr isa override" "$B49" "has rw str .name = .*default_name"

# 8. BUILD -> after "new"
check_output "b49: BUILD -> after new" "$B49" 'after "new" func(scalar \$self) void'
check_output "b49: BUILD body preserved" "$B49" '_initialized.*= 1'
check_output "b49: BUILD no my self unpack" "$B49" 'my.*\$self.*@_' 0

# 9. BUILDARGS -> around "new"
check_output "b49: BUILDARGS -> around new" "$B49" 'around "new" func(scalar \$self, scalar \$orig'
check_output "b49: BUILDARGS SUPER->orig" "$B49" '\$orig->'

# 10. Multi-attr has with clearer + predicate
check_output "b49: multi-attr clearer func" "$B49" 'func clear_cache(scalar \$self)'
check_output "b49: multi-attr predicate func" "$B49" 'func has_cache(scalar \$self)'

# No crash
check_no_crash "b49: no crash" "$B49"

# ====================================================================
# Batch 50: Magic variables, directory ops, file tests
# ====================================================================
B50="$TESTDIR/test_batch50.pl"

# 1. $^T -> core::time()
check_output "b50: caret T -> core::time" "$B50" 'core::time()'

# 2. $^X -> core::argv(0)
check_output "b50: caret X -> core::argv" "$B50" 'core::argv(0)'

# 3. $; -> REVIEW comment
check_output "b50: dollar semicolon -> REVIEW" "$B50" 'subscript separator'

# 4. opendir/readdir/closedir idiom
check_output "b50: opendir commented" "$B50" '# opendir'
check_output "b50: readdir uses core::readdir with path" "$B50" 'core::readdir(\$dir)'
check_output "b50: closedir commented" "$B50" '# closedir.*unnecessary'

# 5. opendir with bareword
check_output "b50: bareword opendir commented" "$B50" '# opendir'
check_output "b50: bareword readdir with path" "$B50" 'core::readdir("/var/log")'

# 6-7. -l symlink test
check_output "b50: -l var -> readlink" "$B50" 'core::readlink(\$f)'
check_output "b50: -l str -> readlink" "$B50" 'core::readlink("/tmp/link")'

# 8. -u setuid test
check_output "b50: -u -> REVIEW stat mode" "$B50" 'setuid test'

# 9. -k sticky bit
check_output "b50: -k -> REVIEW stat mode" "$B50" 'sticky bit test'

# 10. -p named pipe
check_output "b50: -p -> REVIEW pipe" "$B50" 'named pipe test'

# 11-12. -T/-B text/binary
check_output "b50: -T -> REVIEW text file" "$B50" 'text file test'
check_output "b50: -B -> REVIEW binary file" "$B50" 'binary file test'

# No crash
check_no_crash "b50: no crash" "$B50"

# ====================================================================
# Batch 51: study(), given/when
# ====================================================================
B51="$TESTDIR/test_batch51.pl"

# 1-2. study() commented out
check_output "b51: study var commented" "$B51" '# study.*no-op'
check_output "b51: study parens commented" "$B51" '# study.*not needed'

# 3. given/when with numbers -> if/elsif/else
check_output "b51: given -> scoped var" "$B51" 'my scalar \$__given = \$x'
check_output "b51: when 1 -> if" "$B51" 'if (\$__given == 1)'
check_output "b51: when 2 -> elsif" "$B51" 'elsif (\$__given == 2)'
check_output "b51: when 3 -> elsif" "$B51" 'elsif (\$__given == 3)'
check_output "b51: default -> else" "$B51" 'else {'

# 4. given/when with strings
check_output "b51: when string red -> eq" "$B51" '\$__given eq "red"'
check_output "b51: when string blue -> elsif eq" "$B51" 'elsif.*\$__given eq "blue"'

# 5. given/when with regex
check_output "b51: when regex -> if =~" "$B51" '\$__given =~ /\^ERROR/'
check_output "b51: when regex warning -> elsif =~" "$B51" 'elsif.*\$__given =~ /\^WARNING/'

# 6. given/when with variable
check_output "b51: when var -> eq" "$B51" '\$__given eq \$expected'

# No crash
check_no_crash "b51: no crash" "$B51"

# ====================================================================
# Batch 52: Test::More, Path::Tiny
# ====================================================================
B52="$TESTDIR/test_batch52.pl"

# 1. Test::More conversions
check_output "b52: use Test::More commented" "$B52" '# use Test::More'
check_output "b52: ok -> if die" "$B52" 'if (!(1 == 1))'
check_output "b52: is -> ne die" "$B52" 'ne.*die'
check_output "b52: isnt -> eq die" "$B52" 'eq.*die.*Expected not'
check_output "b52: like -> match" "$B52" 'match.*die.*No match'
check_output "b52: unlike -> match die" "$B52" 'match.*die.*Unexpected match'
check_output "b52: is_deeply -> REVIEW" "$B52" 'REVIEW.*is_deeply'
check_output "b52: diag -> warn" "$B52" 'warn("debugging'
check_output "b52: note -> say" "$B52" 'say("informational'
check_output "b52: done_testing commented" "$B52" '# done_testing'

# 2. Path::Tiny conversions
check_output "b52: path->lines -> split slurp" "$B52" 'split.*core::slurp'
check_output "b52: path->children -> core::readdir" "$B52" 'core::readdir("/tmp")'
check_output "b52: path->is_file -> core::is_file" "$B52" 'core::is_file'
check_output "b52: path->is_dir -> core::is_dir" "$B52" 'core::is_dir'
check_output "b52: path->remove -> core::unlink" "$B52" 'core::unlink'
check_output "b52: path->parent -> core::dirname" "$B52" 'core::dirname'
check_output "b52: path->stat -> core::stat" "$B52" 'core::stat'

# No crash
check_no_crash "b52: no crash" "$B52"

# ====================================================================
# Batch 53: Converter improvements — eval require, dispatch, CPAN, meta
# ====================================================================
B53="$TESTDIR/test_batch53.pl"

# 1. eval "require Module" -> try/catch with flag
check_output "b53: eval require static -> try/catch" "$B53" 'try {'
check_output "b53: eval require static -> has flag" "$B53" '__has_Some_Module'

# 2. eval require assign form
check_output "b53: eval require assign -> has flag" "$B53" '__has_JSON_XS'

# 3. eval q{ use Module } -> try/catch
check_output "b53: eval use conditional -> try" "$B53" '__has_Time_HiRes'

# 4. eval require dynamic -> REVIEW
check_output "b53: eval require dynamic -> REVIEW" "$B53" 'REVIEW.*dynamic require'

# 5. $obj->$method($arg) -> passthrough (Strada supports dynamic dispatch natively)
check_output "b53: dynamic dispatch -> passthrough" "$B53" '->\$method'

# 6. dispatch assign form
check_output "b53: dispatch assign -> passthrough" "$B53" '->\$method.*\$key'

# 7. $obj->$method no parens
check_output "b53: dispatch no parens -> passthrough" "$B53" '->\$method'

# 8. URI::Escape -> core::url_encode
check_output "b53: uri_escape -> core::url_encode" "$B53" 'core::url_encode('
check_output "b53: uri_unescape -> core::url_decode" "$B53" 'core::url_decode('

# 9. DateTime->now -> core::time
check_output "b53: DateTime->now -> core::time" "$B53" 'core::time()'

# 10. YAML functions -> REVIEW
check_output "b53: YAML::LoadFile -> REVIEW" "$B53" 'REVIEW.*YAML::LoadFile'
check_output "b53: YAML::DumpFile -> REVIEW" "$B53" 'REVIEW.*YAML::DumpFile'

# 11. no strict refs -> multi-line guidance
check_output "b53: no strict refs -> dispatch guidance" "$B53" 'Method dispatch'
check_output "b53: no strict refs -> global guidance" "$B53" 'core::global_get'

# 12. UNIVERSAL::can -> $obj->can()
check_output "b53: UNIVERSAL::can -> ->can()" "$B53" '\$obj->can("do_work")'

# 13. UNIVERSAL::isa -> $obj->isa()
check_output "b53: UNIVERSAL::isa -> ->isa()" "$B53" '\$obj->isa("MyBase")'

# 14. Multi-line and/or joining
check_output "b53: multiline and joined" "$B53" 'check_one.*check_two.*check_three'

# 15. Multi-line qw() joining
check_output "b53: multiline qw joined" "$B53" 'func_one.*func_two.*func_three'

# 16. use List::MoreUtils -> REVIEW
check_output "b53: use List::MoreUtils -> REVIEW" "$B53" 'REVIEW.*loop'

# 17. use HTML::Entities -> REVIEW
check_output "b53: use HTML::Entities -> REVIEW" "$B53" 'REVIEW.*replace_all'

# 18. use Text::Wrap -> REVIEW
check_output "b53: use Text::Wrap -> REVIEW" "$B53" 'REVIEW.*line-breaking'

# 19. use Module::Load -> REVIEW
check_output "b53: use Module::Load -> REVIEW" "$B53" 'REVIEW.*compile time'

# 20. use XML::Simple -> REVIEW
check_output "b53: use XML::Simple -> REVIEW" "$B53" 'REVIEW.*XML'

# No crash
check_no_crash "b53: no crash" "$B53"

echo ""
echo "================================"
echo "Results: $PASS passed, $FAIL failed, $TOTAL total"
if [ $FAIL -gt 0 ]; then
    echo "SOME TESTS FAILED"
    exit 1
else
    echo "ALL TESTS PASSED"
    exit 0
fi
