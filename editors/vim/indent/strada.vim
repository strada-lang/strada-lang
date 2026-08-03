" Indent file for Strada
if exists("b:did_indent")
  finish
endif
let b:did_indent = 1

setlocal indentexpr=GetStradaIndent(v:lnum)
setlocal indentkeys=0{,0},0),0],!^F,o,O,e
setlocal nosmartindent

let b:undo_indent = "setlocal indentexpr< indentkeys< smartindent<"

if exists("*GetStradaIndent")
  finish
endif

function! GetStradaIndent(lnum)
  let prev = prevnonblank(a:lnum - 1)
  if prev == 0
    return 0
  endif

  " Strip trailing # comment (crude, but avoids counting braces in comments)
  let pline = substitute(getline(prev), '#.*$', '', '')
  let ind = indent(prev)

  " A line ending with an opener indents the next line
  if pline =~ '[{(\[]\s*$'
    let ind += shiftwidth()
  endif

  " A line starting with a closer dedents itself
  if getline(a:lnum) =~ '^\s*[}\])]'
    let ind -= shiftwidth()
  endif

  return ind < 0 ? 0 : ind
endfunction
