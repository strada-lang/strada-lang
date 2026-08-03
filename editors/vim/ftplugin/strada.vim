" Filetype plugin for Strada
if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

setlocal comments=s1:/*,mb:*,ex:*/,:#
setlocal commentstring=#\ %s
setlocal suffixesadd=.strada

" :make runs the compiler's validation-only mode (fast: no C, no cc).
" Errors are gcc-style file:line:col: msg with source-line + caret snippet
" lines, which the trailing %-G discards.
setlocal makeprg=strada\ --check\ %:S
setlocal errorformat=%f:%l:%c:\ %m,%f:%l:\ %m,%-G%.%#

let b:undo_ftplugin = "setlocal comments< commentstring< suffixesadd< makeprg< errorformat<"
