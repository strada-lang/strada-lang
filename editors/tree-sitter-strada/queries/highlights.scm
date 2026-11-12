; Tree-sitter highlight queries for Strada

; ---------------------------------------------------------------- comments
(comment) @comment
(block_comment) @comment
(pod) @comment

; ---------------------------------------------------------------- literals
(number) @number
(string) @string
(q_string) @string
(qw_list) @string
(heredoc) @string
(heredoc_body) @string
(escape_sequence) @string.escape
(interpolation) @variable
(regex) @string.regexp
(regex_m) @string.regexp
(substitution) @string.regexp
(transliteration) @string.regexp
(diamond) @string.special

; --------------------------------------------------------------- variables
(scalar_variable) @variable
(array_variable) @variable
(hash_variable) @variable
(dereference) @variable
(special_variable) @constant.builtin
(function_ref) @function

; ------------------------------------------------------------------- types
(type) @type
(package_name) @type
(label) @label

; --------------------------------------------------------------- functions
(function_definition name: (function_name) @function)
(extern_function name: (function_name) @function)
(call_expression function: (identifier) @function.call)
(call_expression function: (namespaced_identifier) @function.call)
(method_call method: (identifier) @function.method)

; ---------------------------------------------------------------- keywords
[
  "func" "fn" "package" "extends" "with" "struct" "enum" "version"
  "my" "our" "local" "const" "has" "extern" "private" "async" "await"
  "before" "after" "around" "overload" "inherit"
  "ro" "rw" "required" "lazy" "builder"
  "BEGIN" "END" "__C__"
] @keyword

[
  "if" "elsif" "else" "unless" "switch" "case" "default"
] @keyword.conditional

[
  "while" "until" "for" "foreach" "do" "map" "grep" "sort"
] @keyword.repeat

[
  "return" "next" "last" "redo" "goto"
] @keyword.return

[
  "try" "catch" "finally" "throw"
] @keyword.exception

[
  "use" "no" "import_lib" "import_object" "import_archive" "link_lib" "lib"
] @keyword.import

[
  "undef" "__PACKAGE__" "__FILE__" "__LINE__"
] @constant.builtin

; --------------------------------------------------------------- operators
[
  "=" "+=" "-=" "*=" "/=" ".=" "%=" "**=" "//=" "x="
  "+" "-" "*" "/" "%" "**" "."
  "==" "!=" "<" ">" "<=" ">=" "<=>"
  "&&" "||" "//" "!"
  "=~" "!~" ".." "=>" "->"
  "++" "--" "\\" "~" "?" ":"
  "&" "|" "^" "<<" ">>"
  "eq" "ne" "lt" "gt" "le" "ge" "cmp" "x"
  "-e" "-f" "-d" "-r" "-w" "-x" "-s" "-z" "-l"
] @operator

; ------------------------------------------------------------- punctuation
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
[";" ","] @punctuation.delimiter

; ---------------------------------------------------------------- embedded
(c_block_body) @embedded
