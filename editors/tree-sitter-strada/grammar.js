/**
 * Tree-sitter grammar for the Strada programming language.
 * https://github.com/strada-lang/strada-lang
 *
 * External scanner (src/scanner.c) supplies: heredoc bodies (tag-stack
 * state machine), raw __C__ / extern "C" block bodies (balanced braces,
 * C-string/comment aware), and column-0 POD blocks.
 */

const PREC = {
  ASSIGN: 1,
  TERNARY: 2,
  OR: 3,
  AND: 4,
  RANGE: 5,
  EQUALITY: 6,
  RELATIONAL: 7,
  MATCH: 8,
  ADDITIVE: 9,
  MULTIPLICATIVE: 10,
  POWER: 11,
  UNARY: 12,
  POSTFIX: 13,
};

const TYPES = [
  'int', 'num', 'str', 'scalar', 'array', 'hash', 'void', 'dynamic',
  'int8', 'int16', 'uint8', 'byte', 'uint16', 'uint32', 'uint64',
  'size_t', 'char', 'float', 'double', 'long_double',
];

module.exports = grammar({
  name: 'strada',

  externals: $ => [
    $.heredoc_start,
    $.heredoc_body,
    $.c_block_body,
    $.pod,
  ],

  extras: $ => [
    /\s/,
    $.comment,
    $.block_comment,
    $.pod,
    $.heredoc_body,
  ],

  word: $ => $.identifier,

  rules: {
    source_file: $ => repeat($._statement),

    // ------------------------------------------------------- statements

    _statement: $ => choice(
      $.package_statement,
      $.use_statement,
      $.import_statement,
      $.version_statement,
      $.function_definition,
      $.extern_function,
      $.has_declaration,
      $.method_modifier,
      $.variable_declaration,
      $.destructuring_declaration,
      $.const_declaration,
      $.struct_definition,
      $.enum_definition,
      $.if_statement,
      $.unless_statement,
      $.while_statement,
      $.until_statement,
      $.for_statement,
      $.foreach_statement,
      $.do_while_statement,
      $.try_statement,
      $.return_statement,
      $.throw_statement,
      $.next_statement,
      $.last_statement,
      $.redo_statement,
      $.goto_statement,
      $.switch_statement,
      $.inherit_statement,
      $.begin_block,
      $.end_block,
      $.c_block,
      $.extern_c_block,
      $.labeled_statement,
      $.block,
      $.expression_statement,
      ';',
    ),

    switch_statement: $ => seq(
      'switch', '(', field('value', $._expression), ')',
      '{',
      repeat($.case_clause),
      '}',
    ),

    // Case values are restricted to simple forms: a full expression would
    // let `case $x { ... }` parse the block as a hash subscript on $x.
    case_clause: $ => choice(
      seq('case', field('value', $._case_value), $.block),
      seq('default', $.block),
    ),

    _case_value: $ => choice(
      $.number,
      $.string,
      $.scalar_variable,
      $.namespaced_identifier,
      $.identifier,
      seq('-', $.number),
    ),

    inherit_statement: $ => seq(
      'inherit',
      choice($.argument_list, $.package_name),
      ';',
    ),

    package_statement: $ => seq(
      choice('package', 'extends', 'with'),
      field('name', $.package_name),
      ';',
    ),

    use_statement: $ => seq(
      choice('use', 'no'),
      choice(
        seq('lib', field('path', $.string)),
        seq('overload', $._expression_list),
        seq(field('module', $.package_name), optional($.qw_list)),
        field('path', $.string),
      ),
      ';',
    ),

    import_statement: $ => seq(
      choice('import_lib', 'import_object', 'import_archive', 'link_lib'),
      field('path', $.string),
      ';',
    ),

    version_statement: $ => seq('version', $.string, ';'),

    function_definition: $ => seq(
      optional(choice('private', 'async')),
      optional(choice('private', 'async')),
      choice('func', 'fn'),
      field('name', $.function_name),
      optional(field('parameters', $.parameter_list)),
      optional(field('return_type', $.type)),
      field('body', $.block),
    ),

    extern_function: $ => seq(
      'extern',
      choice('func', 'fn'),
      field('name', $.function_name),
      field('parameters', $.parameter_list),
      optional(field('return_type', $.type)),
      choice(';', field('body', $.block)),
    ),

    method_modifier: $ => seq(
      choice('before', 'after', 'around'),
      field('method', $.string),
      choice('func', 'fn'),
      optional(field('parameters', $.parameter_list)),
      optional(field('return_type', $.type)),
      field('body', $.block),
    ),

    parameter_list: $ => seq(
      '(',
      optional(sepBy(',', $.parameter)),
      ')',
    ),

    parameter: $ => seq(
      optional(field('type', $.type)),
      optional('...'),
      field('name', $._any_variable),
      optional(seq('=', field('default', $._expression))),
    ),

    has_declaration: $ => seq(
      'has',
      choice('ro', 'rw'),
      optional(field('type', $.type)),
      field('name', $.scalar_variable),
      optional(seq('(', sepBy(',', $._has_attribute), ')')),
      optional(seq('=', field('default', $._expression))),
      ';',
    ),

    _has_attribute: $ => choice(
      'required',
      'lazy',
      seq('builder', '=>', $.string),
    ),

    variable_declaration: $ => seq(
      choice('my', 'our', 'local'),
      choice(
        seq(
          optional(field('type', $.type)),
          field('name', $._any_variable),
          optional(choice(
            seq('[', $._expression_list, ']'),
            seq('{', $._expression_list, '}'),
          )),
          optional(seq('=', field('value', $._expression))),
        ),
        seq(
          '(',
          sepBy(',', seq(optional($.type), $._any_variable)),
          ')',
          '=',
          field('value', $._expression),
        ),
      ),
      ';',
    ),

    destructuring_declaration: $ => seq(
      '(',
      'my', optional($.type), $._any_variable,
      repeat(seq(',', optional('my'), optional($.type), $._any_variable)),
      ')',
      '=',
      field('value', $._expression),
      ';',
    ),

    const_declaration: $ => seq(
      'const',
      optional(field('type', $.type)),
      field('name', choice($.identifier, $.scalar_variable)),
      '=',
      field('value', $._expression),
      ';',
    ),

    struct_definition: $ => seq(
      'struct',
      field('name', $.identifier),
      '{',
      repeat(seq(
        field('field_type', $.type),
        field('field_name', choice($.identifier, $.scalar_variable)),
        optional(choice(';', ',')),
      )),
      '}',
    ),

    enum_definition: $ => seq(
      'enum',
      field('name', $.identifier),
      '{',
      sepBy(',', seq($.identifier, optional(seq('=', $._expression)))),
      optional(','),
      '}',
    ),

    if_statement: $ => seq(
      'if', '(', field('condition', $._expression), ')',
      field('consequence', $.block),
      repeat($.elsif_clause),
      optional($.else_clause),
    ),

    unless_statement: $ => seq(
      'unless', '(', field('condition', $._expression), ')',
      field('consequence', $.block),
      repeat($.elsif_clause),
      optional($.else_clause),
    ),

    elsif_clause: $ => seq(
      choice('elsif', seq('else', 'if')),
      '(', field('condition', $._expression), ')',
      $.block,
    ),

    else_clause: $ => seq('else', $.block),

    while_statement: $ => seq(
      'while', '(', field('condition', $._expression), ')', $.block,
    ),

    until_statement: $ => seq(
      'until', '(', field('condition', $._expression), ')', $.block,
    ),

    for_statement: $ => seq(
      'for', '(',
      optional(choice($.variable_declaration, seq($._expression, ';'))),
      optional($._expression), ';',
      optional($._expression),
      ')', $.block,
    ),

    foreach_statement: $ => choice(
      seq(
        'foreach',
        optional(seq(optional('my'), optional($.type), $._any_variable)),
        '(', $._expression_list, ')',
        $.block,
      ),
      // Perl-style `for my $x (@list)` — the variable disambiguates from
      // the C-style for_statement.
      seq(
        'for',
        seq(optional('my'), optional($.type), $._any_variable),
        '(', $._expression_list, ')',
        $.block,
      ),
    ),

    do_while_statement: $ => prec(1, seq(
      'do', $.block,
      choice('while', 'until'),
      '(', $._expression, ')', ';',
    )),

    try_statement: $ => seq(
      'try', $.block,
      repeat($.catch_clause),
      optional($.finally_clause),
    ),

    catch_clause: $ => seq(
      'catch',
      '(',
      optional(field('class', $.package_name)),
      field('variable', $.scalar_variable),
      ')',
      $.block,
    ),

    finally_clause: $ => seq('finally', $.block),

    return_statement: $ => seq(
      'return',
      optional($._expression),
      optional($.statement_modifier),
      ';',
    ),

    throw_statement: $ => seq('throw', $._expression, ';'),

    next_statement: $ => seq(
      'next', optional(alias($.identifier, $.label)),
      optional($.statement_modifier), ';',
    ),

    last_statement: $ => seq(
      'last', optional(alias($.identifier, $.label)),
      optional($.statement_modifier), ';',
    ),

    redo_statement: $ => seq(
      'redo', optional(alias($.identifier, $.label)), ';',
    ),

    goto_statement: $ => seq('goto', alias($.identifier, $.label), ';'),

    begin_block: $ => seq('BEGIN', $.block),
    end_block: $ => seq('END', $.block),

    c_block: $ => seq('__C__', '{', optional($.c_block_body), '}'),

    extern_c_block: $ => seq(
      'extern', '"C"', '{', optional($.c_block_body), '}',
    ),

    labeled_statement: $ => prec(1, seq(
      field('label', alias($.identifier, $.label)),
      ':',
      $._statement,
    )),

    block: $ => seq('{', repeat($._statement), '}'),

    expression_statement: $ => seq(
      $._expression,
      optional($.statement_modifier),
      ';',
    ),

    statement_modifier: $ => seq(
      choice('if', 'unless', 'while', 'until', 'foreach'),
      $._expression,
    ),

    // ------------------------------------------------------ expressions

    _expression: $ => choice(
      $.assignment_expression,
      $.ternary_expression,
      $.binary_expression,
      $.unary_expression,
      $.update_expression,
      $.match_expression,
      $.await_expression,
      $.file_test_expression,
      $.block_call,
      $._postfix_expression,
    ),

    await_expression: $ => prec.right(PREC.UNARY, seq('await', $._expression)),

    file_test_expression: $ => prec.right(PREC.UNARY, seq(
      field('operator', choice('-e', '-f', '-d', '-r', '-w', '-x', '-s', '-z', '-l')),
      $._expression,
    )),

    // map { ... } @list, grep { ... } (1..10), sort { $a <=> $b } @arr,
    // sort @arr / sort keys(%h), plus the plain call forms sort(@a) / map($fn, @a).
    block_call: $ => prec.right(PREC.POSTFIX, choice(
      seq(
        field('function', choice('map', 'grep')),
        choice(
          seq(
            field('block', alias($.brace_body, $.block)),
            optional(field('argument', $._expression)),
          ),
          field('arguments', $.argument_list),
        ),
      ),
      seq(
        field('function', 'sort'),
        choice(
          seq(
            field('block', alias($.brace_body, $.block)),
            optional(field('argument', $._expression)),
          ),
          field('argument', $._expression),
        ),
      ),
    )),

    brace_body: $ => prec(1, seq(
      '{',
      repeat($._statement),
      optional($._expression),
      '}',
    )),

    assignment_expression: $ => prec.right(PREC.ASSIGN, seq(
      field('left', $._expression),
      field('operator', choice(
        '=', '+=', '-=', '*=', '/=', '.=', '%=', '**=', '//=', 'x=',
      )),
      field('right', $._expression),
    )),

    ternary_expression: $ => prec.right(PREC.TERNARY, seq(
      field('condition', $._expression),
      '?', field('consequence', $._expression),
      ':', field('alternative', $._expression),
    )),

    binary_expression: $ => {
      const table = [
        [PREC.OR, choice('||', '//')],
        [PREC.AND, '&&'],
        [PREC.EQUALITY, choice('==', '!=', '<=>', 'eq', 'ne', 'cmp')],
        [PREC.RELATIONAL, choice('<', '>', '<=', '>=', 'lt', 'gt', 'le', 'ge')],
        [PREC.ADDITIVE, choice('+', '-', '.')],
        [PREC.MULTIPLICATIVE, choice('*', '/', '%', 'x')],
        [PREC.RANGE, '..'],
        [PREC.RANGE, choice('&', '|', '^', '<<', '>>')],
      ];
      return choice(
        ...table.map(([p, op]) => prec.left(p, seq(
          field('left', $._expression),
          field('operator', op),
          field('right', $._expression),
        ))),
        prec.right(PREC.POWER, seq(
          field('left', $._expression),
          field('operator', '**'),
          field('right', $._expression),
        )),
      );
    },

    match_expression: $ => prec.left(PREC.MATCH, seq(
      field('left', $._expression),
      field('operator', choice('=~', '!~')),
      field('right', choice($.regex_m, $.regex, $.substitution, $.transliteration, $._expression)),
    )),

    unary_expression: $ => prec.right(PREC.UNARY, seq(
      field('operator', choice('!', '-', '+', '\\', '~')),
      field('operand', $._expression),
    )),

    update_expression: $ => prec.right(PREC.UNARY, choice(
      seq(choice('++', '--'), $._expression),
      prec.left(PREC.POSTFIX, seq($._expression, choice('++', '--'))),
    )),

    _postfix_expression: $ => choice(
      $.call_expression,
      $.method_call,
      $.arrow_index,
      $.element_access,
      $.slice_access,
      $._primary_expression,
    ),

    call_expression: $ => prec(PREC.POSTFIX, seq(
      field('function', choice(
        $.identifier,
        $.namespaced_identifier,
        // ::func() — current-package-qualified call
        alias(token(/::[A-Za-z_][A-Za-z0-9_]*/), $.namespaced_identifier),
      )),
      field('arguments', $.argument_list),
    )),

    method_call: $ => prec.left(PREC.POSTFIX, seq(
      field('object', $._postfix_expression),
      '->',
      field('method', choice($.identifier, $.scalar_variable)),
      optional(field('arguments', $.argument_list)),
    )),

    arrow_index: $ => prec.left(PREC.POSTFIX, seq(
      field('object', $._postfix_expression),
      '->',
      choice(
        seq('{', $._expression, '}'),
        seq('[', $._expression, ']'),
        $.argument_list,
      ),
    )),

    element_access: $ => prec(PREC.POSTFIX, seq(
      field('object', choice(
        $._sigil_variable, $.element_access, $.arrow_index, $.method_call,
        $.call_expression,
      )),
      choice(
        seq('{', $._expression, '}'),
        seq('[', $._expression, ']'),
      ),
    )),

    slice_access: $ => prec(PREC.POSTFIX + 1, seq(
      field('object', choice($.array_variable, $.hash_variable, $.dereference)),
      choice(
        seq('[', $._expression_list, ']'),
        seq('{', $._expression_list, '}'),
      ),
    )),

    argument_list: $ => seq(
      '(',
      optional(sepBy(',', choice(seq('...', $._expression), $._expression))),
      ')',
    ),

    _expression_list: $ => prec.left(seq(
      $._expression,
      repeat(seq(',', $._expression)),
      optional(','),
    )),

    _primary_expression: $ => choice(
      $.number,
      $.string,
      $.q_string,
      $.qw_list,
      $.heredoc,
      $.regex_m,
      $.regex,
      $.substitution,
      $.transliteration,
      $.function_ref,
      $._sigil_variable,
      $.special_variable,
      $.anonymous_array,
      $.anonymous_hash,
      $.anonymous_function,
      $.do_block,
      $.diamond,
      $.parenthesized,
      $.fat_comma_pair,
      $.identifier,
      $.namespaced_identifier,
      'undef',
      '__PACKAGE__',
      '__FILE__',
      '__LINE__',
    ),

    parenthesized: $ => seq('(', optional($._expression_list), ')'),

    fat_comma_pair: $ => prec.right(PREC.ASSIGN, seq(
      field('key', $._expression),
      '=>',
      field('value', $._expression),
    )),

    anonymous_array: $ => seq('[', optional($._expression_list), ']'),

    anonymous_hash: $ => prec(-1, seq(
      '{', optional($._expression_list), '}',
    )),

    anonymous_function: $ => seq(
      choice('func', 'fn'),
      optional($.parameter_list),
      optional($.type),
      $.block,
    ),

    do_block: $ => seq('do', $.block),

    diamond: $ => token(/<\$?[A-Za-z_][A-Za-z0-9_]*>/),

    heredoc: $ => $.heredoc_start,

    // --------------------------------------------------------- variables

    _sigil_variable: $ => choice(
      $.scalar_variable,
      $.array_variable,
      $.hash_variable,
      $.dereference,
    ),

    _any_variable: $ => choice(
      $.scalar_variable,
      $.array_variable,
      $.hash_variable,
    ),

    scalar_variable: $ => token(/\$[A-Za-z_][A-Za-z0-9_]*|\$[0-9]+|\$_|\$&/),
    array_variable: $ => token(/@[A-Za-z_][A-Za-z0-9_]*|@_/),
    hash_variable: $ => token(/%[A-Za-z_][A-Za-z0-9_]*/),

    dereference: $ => prec(PREC.UNARY, choice(
      token(/\$\$[A-Za-z_][A-Za-z0-9_]*/),
      token(/@\$[A-Za-z_][A-Za-z0-9_]*/),
      token(/%\$[A-Za-z_][A-Za-z0-9_]*/),
      seq(token(/[$@%]\{/), $._expression, '}'),
      seq(token(/\$\$\{/), $._expression, '}'),
    )),

    special_variable: $ => token(choice('STDIN', 'STDOUT', 'STDERR')),

    // ----------------------------------------------------------- lexical

    identifier: $ => /[A-Za-z_][A-Za-z0-9_]*/,

    namespaced_identifier: $ => /[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)+/,

    // Composed from the identifier/word token: a parallel regex token would
    // lose to word-token keyword extraction and never lex.
    package_name: $ => choice($.identifier, $.namespaced_identifier),

    function_name: $ => prec(1, choice($.identifier, $.namespaced_identifier)),

    type: $ => choice(...TYPES, $.identifier),

    number: $ => token(choice(
      /0[xX][0-9a-fA-F]+/,
      /\d+(\.\d+)?([eE][+-]?\d+)?/,
    )),

    // Every token INSIDE a string is immediate: extras (comments, POD,
    // heredoc bodies) must never fire between string-content tokens — a
    // '#' in string content would otherwise start a comment that swallows
    // the closing quote.
    // Content tokens carry prec 2 — above the comment tokens' prec — so a
    // literal "/*" or "#" inside a string can never lose the lexical race
    // to a comment extra (which would swallow the closing quote).
    string: $ => choice(
      seq(
        '"',
        repeat(choice(
          $.escape_sequence,
          $.interpolation,
          token.immediate(prec(2, /[^"\\$@]+/)),
          token.immediate(prec(2, /[$@]/)),
        )),
        token.immediate('"'),
      ),
      seq(
        "'",
        repeat(choice(
          token.immediate(prec(2, /\\./)),
          token.immediate(prec(2, /[^'\\]+/)),
        )),
        token.immediate("'"),
      ),
    ),

    escape_sequence: $ => token.immediate(/\\x\{[0-9a-fA-F]+\}|\\x[0-9a-fA-F]{1,2}|\\[0-7]{1,3}|\\./),

    interpolation: $ => token.immediate(choice(
      /\$\{[A-Za-z_][A-Za-z0-9_]*\}/,
      /\$[A-Za-z_][A-Za-z0-9_]*(->(\{[^}]*\}|\[[^\]]*\]))*/,
      /@[A-Za-z_][A-Za-z0-9_]*/,
    )),

    // Paired delimiters support one nesting level (qq(a (b) c)), matching
    // Perl's balanced-delimiter behavior for the practical cases.
    q_string: $ => token(choice(
      seq(choice('q', 'qq'), /\s*\(/, /([^()\\]|\\.|\(([^()\\]|\\.)*\))*/, ')'),
      seq(choice('q', 'qq'), /\s*\{/, /([^{}\\]|\\.|\{([^{}\\]|\\.)*\})*/, '}'),
      seq(choice('q', 'qq'), /\s*\[/, /([^\[\]\\]|\\.|\[([^\[\]\\]|\\.)*\])*/, ']'),
      seq(choice('q', 'qq'), /\s*</, /([^<>\\]|\\.|<([^<>\\]|\\.)*>)*/, '>'),
      seq(choice('q', 'qq'), '/', /([^\/\\]|\\.)*/, '/'),
      seq(choice('q', 'qq'), '|', /([^|\\]|\\.)*/, '|'),
      seq(choice('q', 'qq'), '!', /([^!\\]|\\.)*/, '!'),
    )),

    qw_list: $ => token(choice(
      seq('qw', /\s*\(/, /[^)]*/, ')'),
      seq('qw', /\s*\[/, /[^\]]*/, ']'),
      seq('qw', /\s*\{/, /[^}]*/, '}'),
      seq('qw', /\s*</, /[^>]*/, '>'),
      seq('qw', '/', /[^\/]*/, '/'),
      seq('qw', '!', /[^!]*/, '!'),
      seq('qw', '|', /[^|]*/, '|'),
    )),

    function_ref: $ => token(/\\&[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*/),

    regex_m: $ => token(choice(
      seq('m', '/', /([^\/\\\n]|\\.)*/, '/', /[gimsx]*/),
      seq('m', '{', /([^{}\\]|\\.)*/, '}', /[gimsx]*/),
      seq('m', '(', /([^()\\]|\\.)*/, ')', /[gimsx]*/),
      seq('m', '[', /([^\[\]\\]|\\.)*/, ']', /[gimsx]*/),
      seq('m', '!', /([^!\\\n]|\\.)*/, '!', /[gimsx]*/),
      seq('m', '|', /([^|\\\n]|\\.)*/, '|', /[gimsx]*/),
      seq('m', '#', /([^#\\\n]|\\.)*/, '#', /[gimsx]*/),
    )),

    // First char excludes '*' so '/*' always lexes as a block comment.
    regex: $ => token(seq('/', /[^*\/\\\n]|\\./, /([^\/\\\n]|\\.)*/, '/', /[gimsx]*/)),

    substitution: $ => token(choice(
      seq('s', '/', /([^\/\\\n]|\\.)*/, '/', /([^\/\\\n]|\\.)*/, '/', /[gimsxe]*/),
      seq('s', '{', /([^{}\\]|\\.)*/, '}', /\s*\{/, /([^{}\\]|\\.)*/, '}', /[gimsxe]*/),
      seq('s', '(', /([^()\\]|\\.)*/, ')', /\s*\(/, /([^()\\]|\\.)*/, ')', /[gimsxe]*/),
      seq('s', '[', /([^\[\]\\]|\\.)*/, ']', /\s*\[/, /([^\[\]\\]|\\.)*/, ']', /[gimsxe]*/),
      seq('s', '!', /([^!\\\n]|\\.)*/, '!', /([^!\\\n]|\\.)*/, '!', /[gimsxe]*/),
      seq('s', '|', /([^|\\\n]|\\.)*/, '|', /([^|\\\n]|\\.)*/, '|', /[gimsxe]*/),
      seq('s', '#', /([^#\\\n]|\\.)*/, '#', /([^#\\\n]|\\.)*/, '#', /[gimsxe]*/),
    )),

    transliteration: $ => token(seq(
      choice('tr', 'y'), '/',
      /([^\/\\\n]|\\.)*/, '/',
      /([^\/\\\n]|\\.)*/, '/',
      /[cdsr]*/,
    )),

    comment: $ => token(/#.*/),

    block_comment: $ => token(prec(1, seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'))),
  },
});

function sepBy1(sep, rule) {
  return seq(rule, repeat(seq(sep, rule)));
}

function sepBy(sep, rule) {
  return sepBy1(sep, rule);
}
