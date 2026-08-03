;;; strada-mode.el --- Major mode for the Strada programming language -*- lexical-binding: t; -*-

;; Maintainer: Strada Language (https://github.com/strada-lang/strada-lang)
;; Keywords: languages
;; Version: 1.0
;; Package-Requires: ((emacs "25.1"))

;; This file is part of the Strada Language.
;; Distributed under the GNU General Public License, version 2.

;;; Commentary:

;; A major mode for editing Strada source (.strada): syntax highlighting,
;; simple brace-based indentation, and compiler integration.
;;
;;   C-c C-c  runs `strada --check' on the current file (validation only —
;;            no C generated, no cc).  Errors are gcc-style
;;            `file:line:col: message', which compilation-mode's built-in
;;            GNU pattern matches out of the box, so next-error jumps
;;            straight to the offending line/column.
;;
;; Install:
;;   (add-to-list 'load-path "/path/to/strada-lang/editors/emacs")
;;   (require 'strada-mode)

;;; Code:

(defgroup strada nil
  "Major mode for the Strada programming language."
  :group 'languages)

(defcustom strada-indent-offset 4
  "Indentation width for `strada-mode'."
  :type 'integer
  :safe 'integerp
  :group 'strada)

(defcustom strada-program "strada"
  "Command used to compile/check Strada programs."
  :type 'string
  :group 'strada)

;; ---------------------------------------------------------------- syntax

(defvar strada-mode-syntax-table
  (let ((table (make-syntax-table)))
    ;; # line comments (style a), /* */ block comments (style b)
    (modify-syntax-entry ?# "<" table)
    (modify-syntax-entry ?\n ">" table)
    (modify-syntax-entry ?/ ". 14" table)
    (modify-syntax-entry ?* ". 23b" table)
    ;; Strings
    (modify-syntax-entry ?\" "\"" table)
    (modify-syntax-entry ?' "\"" table)
    (modify-syntax-entry ?\\ "\\" table)
    ;; Symbols and sigils
    (modify-syntax-entry ?_ "_" table)
    (modify-syntax-entry ?$ "'" table)
    (modify-syntax-entry ?@ "'" table)
    (modify-syntax-entry ?% "'" table)
    table)
  "Syntax table for `strada-mode'.")

;; ------------------------------------------------------------- font-lock

(defconst strada--keywords
  '("func" "fn" "package" "extends" "with" "enum" "version" "return"
    "async" "await" "bless" "overload" "before" "after" "around"
    "tie" "untie" "tied" "goto" "if" "elsif" "else" "unless" "while"
    "until" "for" "foreach" "do" "next" "last" "redo" "try" "catch"
    "finally" "throw" "die" "warn" "use" "no" "import_lib"
    "import_object" "import_archive" "link_lib" "BEGIN" "END"))

(defconst strada--storage
  '("my" "our" "local" "const" "has" "extern" "private"
    "ro" "rw" "required" "lazy" "builder"))

(defconst strada--types
  '("int" "num" "str" "scalar" "array" "hash" "void" "dynamic"
    "int8" "int16" "uint8" "byte" "uint16" "uint32" "uint64" "size_t"
    "char" "float" "double"))

(defconst strada--builtins
  '("say" "print" "printf" "sprintf" "push" "pop" "shift" "unshift"
    "splice" "keys" "values" "each" "exists" "delete" "defined" "ref"
    "length" "substr" "index" "rindex" "join" "split" "map" "grep"
    "sort" "reverse" "chomp" "chop" "uc" "lc" "ucfirst" "lcfirst"
    "trim" "scalar" "size" "wantarray" "sleep" "chr" "ord" "hex" "oct"
    "abs" "slurp" "spew" "open" "close" "eof" "read" "seek" "tell"
    "select" "bytes" "char_at" "byte_at" "isa" "can"))

(defconst strada--constants
  '("undef" "STDIN" "STDOUT" "STDERR" "__PACKAGE__" "__FILE__"))

(defconst strada-font-lock-keywords
  `(;; func NAME / fn NAME
    ("\\_<\\(?:func\\|fn\\)\\s-+\\(\\(?:\\sw\\|::\\)+\\)"
     1 font-lock-function-name-face)
    ;; package/extends/with NAME
    ("\\_<\\(?:package\\|extends\\|with\\)\\s-+\\([[:alnum:]_:]+\\)"
     1 font-lock-type-face)
    ;; Namespaced builtins: core::foo, re::match, c::alloc, ...
    ("\\_<\\(?:core\\|sys\\|math\\|async\\|utf8\\|usb\\|ssl\\|re\\|str\\|sb\\|thread\\|c\\)::[[:alnum:]_]+"
     . font-lock-builtin-face)
    (,(regexp-opt strada--keywords 'symbols) . font-lock-keyword-face)
    (,(regexp-opt strada--storage 'symbols) . font-lock-keyword-face)
    (,(regexp-opt strada--types 'symbols) . font-lock-type-face)
    (,(regexp-opt strada--builtins 'symbols) . font-lock-builtin-face)
    (,(regexp-opt strada--constants 'symbols) . font-lock-constant-face)
    ;; Word operators
    (,(regexp-opt '("eq" "ne" "lt" "gt" "le" "ge" "cmp" "x") 'symbols)
     . font-lock-keyword-face)
    ;; __C__ marker
    ("\\_<__C__\\_>" . font-lock-preprocessor-face)
    ;; Variables: $x @x %x, ${x}, $$ref, $1..$9, $_, @_
    ("[$@%]\\(?:[[:alpha:]_][[:alnum:]_]*\\|{[[:alnum:]_]+}\\|[0-9]+\\|_\\)"
     . font-lock-variable-name-face)
    ("\\$\\$[[:alpha:]_][[:alnum:]_]*" . font-lock-variable-name-face))
  "Font-lock keywords for `strada-mode'.")

;; ------------------------------------------------------------ indentation

(defun strada-indent-line ()
  "Indent the current line by brace/paren depth.
Lines whose first character closes a block dedent one level.  Lines
inside strings or block comments are left alone."
  (interactive)
  (let* ((ppss (syntax-ppss (line-beginning-position)))
         (depth (car ppss)))
    (if (nth 8 ppss)
        'noindent                       ; inside a string or /* */ comment
      (let ((indent (* strada-indent-offset depth)))
        (save-excursion
          (back-to-indentation)
          (when (looking-at "[])}]")
            (setq indent (max 0 (- indent strada-indent-offset)))))
        (if (<= (current-column) (current-indentation))
            (indent-line-to indent)
          (save-excursion (indent-line-to indent)))))))

;; --------------------------------------------------------------- commands

(defun strada-check ()
  "Run `strada --check' (validation only) on the current file."
  (interactive)
  (unless buffer-file-name
    (user-error "Buffer is not visiting a file"))
  (when (buffer-modified-p)
    (save-buffer))
  (compile (concat strada-program " --check "
                   (shell-quote-argument buffer-file-name))))

(defun strada-run ()
  "Compile and run the current file with `strada -r'."
  (interactive)
  (unless buffer-file-name
    (user-error "Buffer is not visiting a file"))
  (when (buffer-modified-p)
    (save-buffer))
  (compile (concat strada-program " -r "
                   (shell-quote-argument buffer-file-name))))

(defvar strada-mode-map
  (let ((map (make-sparse-keymap)))
    (define-key map (kbd "C-c C-c") #'strada-check)
    (define-key map (kbd "C-c C-r") #'strada-run)
    map)
  "Keymap for `strada-mode'.")

;; ------------------------------------------------------------------ mode

;;;###autoload
(define-derived-mode strada-mode prog-mode "Strada"
  "Major mode for editing Strada source code.

\\{strada-mode-map}"
  :syntax-table strada-mode-syntax-table
  (setq-local comment-start "# ")
  (setq-local comment-end "")
  (setq-local comment-start-skip "\\(?:#+\\|/\\*+\\)\\s-*")
  (setq-local font-lock-defaults '(strada-font-lock-keywords))
  (setq-local indent-line-function #'strada-indent-line)
  (setq-local electric-indent-chars
              (append "{}()" electric-indent-chars))
  (setq-local compile-command
              (concat strada-program " --check "
                      (when buffer-file-name
                        (shell-quote-argument buffer-file-name)))))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.strada\\'" . strada-mode))

(provide 'strada-mode)
;;; strada-mode.el ends here
