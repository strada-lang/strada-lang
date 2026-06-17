#!/usr/bin/env python3
"""Split a generated Combined.c into N parts + a shared header for parallel
compilation (Route-B parallel build of the self-hosting compiler).

The generated C is a single ~4.5MB translation unit. gcc cost is roughly the
sum over functions, so once no single function is huge (see the gen_expression
breakup) the build is aggregate-bound; splitting into N independent .c files and
compiling them with `make -jN` turns that aggregate into wall-clock = total/N.

The split is behaviour-preserving: concatenating all emitted function bodies
reproduces the original program. Correctness relies on the generated C having:
  - no file-scope `static` *variables* (only static functions) — verified by the
    Strada codegen; static functions are de-static'd + forward-declared so they
    remain callable across parts;
  - all globals as simple `TYPE NAME = ...;` definitions (emitted once in part 0,
    `extern` in the header).

Brace counting is tokenizer-aware (skips string/char literals and comments), so
string literals containing unbalanced braces (the codegen emits plenty) do not
desync block detection. Usage: split_combined.py <Combined.c> <out_dir> <N>
"""
import sys, os

def main():
    src_path, out_dir, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
    os.makedirs(out_dir, exist_ok=True)
    raw = open(src_path).read().split('\n')

    # Preprocessor lines (#include/#define/...) have no ';' terminator, so pull
    # them out first and blank them from the body before segmenting.
    includes = [l for l in raw if l.lstrip().startswith('#')]
    body = '\n'.join('' if l.lstrip().startswith('#') else l for l in raw)

    # Tokenizer-aware segmentation: emit a segment at every depth-0 ';' (a
    # statement) or depth-0 closing '}' (a block), skipping strings/comments.
    segs = []
    i = 0; m = len(body); depth = 0; seg = 0; st = 'n'
    while i < m:
        c = body[i]; d = body[i+1] if i+1 < m else ''
        if st == 'n':
            if c == '/' and d == '*': st = 'bc'; i += 2; continue
            if c == '/' and d == '/': st = 'lc'; i += 1; continue
            if c == '"': st = 's'; i += 1; continue
            if c == "'": st = 'c'; i += 1; continue
            if c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    j = i + 1
                    while j < m and body[j] in ' \t': j += 1
                    if j < m and body[j] == '\n': j += 1
                    segs.append((body[seg:j], True)); seg = j; i = j; continue
            elif c == ';' and depth == 0:
                j = i + 1
                while j < m and body[j] in ' \t': j += 1
                if j < m and body[j] == '\n': j += 1
                segs.append((body[seg:j], False)); seg = j; i = j; continue
        elif st == 'bc':
            if c == '*' and d == '/': st = 'n'; i += 2; continue
        elif st == 'lc':
            if c == '\n': st = 'n'
        elif st == 's':
            if c == '\\': i += 2; continue
            if c == '"': st = 'n'
        elif st == 'c':
            if c == '\\': i += 2; continue
            if c == "'": st = 'n'
        i += 1
    if seg < m and body[seg:].strip():
        segs.append((body[seg:], False))

    header = list(includes)
    globals_def = []     # global/static variable definitions (go in part 0)
    funcs = []           # function-definition texts
    for text, is_block in segs:
        s = text.strip()
        if not s:
            continue
        if is_block:
            sig = s[:s.find('{')]
            if s.startswith(('typedef', 'struct ', 'enum ', 'union ')) or s.rstrip().endswith('};'):
                header.append(text)                      # type definition
            else:                                        # function definition
                if sig.lstrip().startswith('static '):
                    text = text.replace('static ', '', 1)
                    header.append(text[:text.find('{')].rstrip() + ';')  # de-static fwd decl
                funcs.append(text)
        else:
            decl = s[:-1] if s.endswith(';') else s
            head = decl.split('=')[0] if '=' in decl else decl
            if '=' in decl and '(' not in head:         # global variable definition
                if head.lstrip().startswith('static '):
                    head = head.replace('static ', '', 1)
                    text = text.replace('static ', '', 1)
                header.append('extern ' + head.rstrip() + ';')
                globals_def.append(text)
            elif decl.lstrip().startswith('static ') and '(' not in head:  # static var, no init
                head2 = head.replace('static ', '', 1)
                header.append('extern ' + head2.rstrip() + ';')
                globals_def.append(text.replace('static ', '', 1))
            elif '(' in decl and ')' in decl:           # forward declaration
                header.append(text)
            else:
                header.append(text)                      # any other file-scope line

    # Bin-pack functions into N parts balanced by size (greedy: largest first
    # into the currently-smallest bin) so per-part compile times are even.
    parts = [[] for _ in range(n)]; sizes = [0] * n
    for text in sorted(funcs, key=len, reverse=True):
        k = sizes.index(min(sizes))
        parts[k].append(text); sizes[k] += len(text)

    open(os.path.join(out_dir, 'csplit.h'), 'w').write('\n'.join(header) + '\n')
    for k in range(n):
        out = ['#include "csplit.h"', '']
        if k == 0:
            out += globals_def + ['']
        out += parts[k]
        open(os.path.join(out_dir, f'part_{k}.c'), 'w').write('\n'.join(out) + '\n')
    print(f"split {src_path} -> {n} parts ({len(funcs)} funcs, "
          f"{len(globals_def)} globals); part bytes={[s for s in sizes]}")

if __name__ == '__main__':
    main()
