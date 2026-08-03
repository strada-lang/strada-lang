" Vim syntax file
" Language:     Strada
" Maintainer:   Strada Language (https://github.com/strada-lang/strada-lang)
" Filenames:    *.strada

if exists("b:current_syntax")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

" Embedded C for __C__ { ... } blocks
syntax include @stradaC syntax/c.vim
unlet! b:current_syntax

" ---------------------------------------------------------------- Comments
syn match   stradaComment "#.*$" contains=stradaTodo
syn region  stradaComment start="/\*" end="\*/" contains=stradaTodo
syn keyword stradaTodo TODO FIXME XXX NOTE contained
syn region  stradaPod start="^=\a\w*" end="^=cut\>" keepend

" ------------------------------------------------------------------ Types
syn keyword stradaType int num str scalar array hash void dynamic
syn keyword stradaType int8 int16 uint8 byte uint16 uint32 uint64 size_t
syn keyword stradaType char float double

" --------------------------------------------------------------- Keywords
syn keyword stradaStorage my our local const has extern private state
syn keyword stradaKeyword func fn package extends with enum version
syn keyword stradaKeyword async await return bless overload
syn keyword stradaKeyword before after around
syn keyword stradaKeyword tie untie tied goto
syn keyword stradaAttribute ro rw required lazy builder
syn keyword stradaConditional if elsif else unless
syn keyword stradaRepeat while until for foreach do
syn keyword stradaStatement next last redo
syn keyword stradaException try catch finally throw die warn
syn keyword stradaInclude use no import_lib import_object import_archive link_lib
syn keyword stradaBoolean undef
syn keyword stradaSpecialBlock BEGIN END
syn keyword stradaConstant STDIN STDOUT STDERR
syn match   stradaConstant "\<__PACKAGE__\>\|\<__FILE__\>\|\<__LINE__\>"

" String comparison / misc word operators
syn keyword stradaOperator eq ne lt gt le ge cmp x isa can

" --------------------------------------------------------------- Builtins
syn keyword stradaBuiltin say print printf sprintf push pop shift unshift
syn keyword stradaBuiltin splice keys values each exists delete defined ref
syn keyword stradaBuiltin length substr index rindex join split map grep sort
syn keyword stradaBuiltin reverse chomp chop uc lc ucfirst lcfirst trim
syn keyword stradaBuiltin scalar size wantarray sleep chr ord hex oct abs
syn keyword stradaBuiltin sprintf slurp spew open close eof read seek tell
syn keyword stradaBuiltin select bytes char_at byte_at
" Namespaced builtins: core::, math::, async::, c::, utf8::, usb::, ssl::,
" re::, str::, sb::, sys::, thread::
syn match   stradaBuiltin "\<\%(core\|sys\|math\|async\|utf8\|usb\|ssl\|re\|str\|sb\|thread\|c\)::\h\w*\>"

" ------------------------------------------------------- Function definition
syn match stradaFuncDef "\%(\<\%(func\|fn\)\s\+\)\@<=\h\%(\w\|::\)*"
syn match stradaPackageName "\%(\<\%(package\|extends\|with\)\s\+\)\@<=\h\%(\w\|::\)*"

" -------------------------------------------------------------- Variables
syn match stradaScalar "\$\h\w*\>"
syn match stradaScalar "\$\d\+\>"
syn match stradaScalar "\$[_&]\>"
syn match stradaScalar "\$\$\h\w*\>"
syn match stradaScalar "\${\h\w*}"
syn match stradaArrayVar "@\h\w*\>"
syn match stradaArrayVar "@[_$]\>"
syn match stradaArrayVar "@{\$\?\h\w*}"
syn match stradaHashVar "%\h\w*\>"
syn match stradaHashVar "%{\$\?\h\w*}"

" ---------------------------------------------------------------- Numbers
syn match stradaNumber "\<\d\+\%(\.\d\+\)\?\%([eE][+-]\?\d\+\)\?\>"
syn match stradaNumber "\<0[xX]\x\+\>"

" ---------------------------------------------------------------- Strings
syn match  stradaEscape +\\[ntr0\\"']+ contained
syn match  stradaEscape "\\x\x\{1,2}" contained
syn match  stradaInterp "\$\h\w*\%(->\%({[^}]*}\|\[[^]]*\]\)\)*" contained
syn match  stradaInterp "\${\h\w*}" contained
syn match  stradaInterp "@\h\w*" contained
syn region stradaString start=+"+ skip=+\\"+ end=+"+ contains=stradaEscape,stradaInterp
syn region stradaString start=+'+ skip=+\\'+ end=+'+
syn region stradaString matchgroup=stradaStringDelim start="\<qq\s*(" end=")" contains=stradaEscape,stradaInterp
syn region stradaString matchgroup=stradaStringDelim start="\<qq\s*{" end="}" contains=stradaEscape,stradaInterp
syn region stradaString matchgroup=stradaStringDelim start="\<q\s*(" end=")"
syn region stradaString matchgroup=stradaStringDelim start="\<q\s*{" end="}"
syn region stradaQw     matchgroup=stradaStringDelim start="\<qw\s*(" end=")"

" Heredocs: <<EOT, <<"EOT" (interpolating), <<'EOT' (literal)
syn region stradaHeredoc matchgroup=stradaStringDelim start=+<<\z(\h\w*\)+ end=+^\z1$+ contains=stradaEscape,stradaInterp
syn region stradaHeredoc matchgroup=stradaStringDelim start=+<<"\z(\h\w*\)"+ end=+^\z1$+ contains=stradaEscape,stradaInterp
syn region stradaHeredoc matchgroup=stradaStringDelim start=+<<'\z(\h\w*\)'+ end=+^\z1$+

" Diamond reads: <$fh>, <STDIN>
syn match stradaDiamond "<\$\?\h\w*>"

" ------------------------------------------------------------------ Regex
" m//, s///, tr///, y/// forms anywhere; bare /.../ only after =~ or !~
syn match stradaMatchOp "=\~\|!\~" nextgroup=stradaRegexBare skipwhite
syn match stradaRegexBare "/\%(\\.\|[^/\\]\)*/[gimsx]*" contained contains=stradaInterp
syn match stradaRegex "\<m/\%(\\.\|[^/\\]\)*/[gimsx]*" contains=stradaInterp
syn match stradaSubst "\<s/\%(\\.\|[^/\\]\)*/\%(\\.\|[^/\\]\)*/[gimsxe]*" contains=stradaInterp
syn match stradaTrans "\<\%(tr\|y\)/\%(\\.\|[^/\\]\)*/\%(\\.\|[^/\\]\)*/[cdsr]*"

" ------------------------------------------------------------ __C__ blocks
" Nested braces are consumed by the contained nest region, so the outer
" region's end matches only the block's own closing brace.
syn region stradaCBlockNest start="{" end="}" transparent contained contains=@stradaC,stradaCBlockNest
syn region stradaCBlock matchgroup=stradaCBlockDelim start="\<__C__\>\s*{" end="}" contains=@stradaC,stradaCBlockNest keepend extend

" ------------------------------------------------------------------ Labels
syn match stradaLabel "^\s*\u[A-Z0-9_]*\s*:\%(:\)\@!"

" -------------------------------------------------------------- Highlight
hi def link stradaComment      Comment
hi def link stradaTodo         Todo
hi def link stradaPod          Comment
hi def link stradaType         Type
hi def link stradaStorage      StorageClass
hi def link stradaKeyword      Keyword
hi def link stradaAttribute    StorageClass
hi def link stradaConditional  Conditional
hi def link stradaRepeat       Repeat
hi def link stradaStatement    Statement
hi def link stradaException    Exception
hi def link stradaInclude      Include
hi def link stradaBoolean      Constant
hi def link stradaSpecialBlock PreProc
hi def link stradaConstant     Constant
hi def link stradaOperator     Operator
hi def link stradaBuiltin      Function
hi def link stradaFuncDef      Function
hi def link stradaPackageName  Type
hi def link stradaScalar       Identifier
hi def link stradaArrayVar     Identifier
hi def link stradaHashVar      Identifier
hi def link stradaNumber       Number
hi def link stradaString       String
hi def link stradaStringDelim  Special
hi def link stradaQw           String
hi def link stradaHeredoc      String
hi def link stradaDiamond      Special
hi def link stradaEscape       SpecialChar
hi def link stradaInterp       Identifier
hi def link stradaMatchOp      Operator
hi def link stradaRegex        String
hi def link stradaRegexBare    String
hi def link stradaSubst        String
hi def link stradaTrans        String
hi def link stradaCBlockDelim  PreProc
hi def link stradaLabel        Label

let b:current_syntax = "strada"

let &cpo = s:cpo_save
unlet s:cpo_save
