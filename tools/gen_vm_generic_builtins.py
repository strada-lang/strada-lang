#!/usr/bin/env python3
"""Regenerate interpreter/vm_generic_builtins.inc.

Harvests (name, runtime C function, arity) triples from the simple emission
patterns in compiler/CodeGenBuiltins.strada, validates each against the
uniform StradaValue* prototypes in runtime/strada_runtime.h, and emits a
dispatch table for the VM's generic runtime-bridge (builtin IDs >=
VM_GENERIC_BID_BASE inside OP_BUILTIN).

Only sys:: and math:: names are included: thread::/async::/c:: builtins need
VM-native semantics (threads, closures, raw pointers) and bare names go
through the VM's own bare-builtin handling.

Run from the repo root: python3 tools/gen_vm_generic_builtins.py
"""
import re
import collections

ENTRIES = {}


EXTRA = {}   # (name, arity) -> cfunc: further arities of a name


def add(names_str, cfunc, arity):
    for n in re.findall(r'"([a-z_0-9:]+)"', names_str):
        ENTRIES.setdefault(n, (cfunc, int(arity)))
        EXTRA.setdefault((n, int(arity)), cfunc)


def harvest(src):
    NAME = r'if \((\$name eq "[a-z_0-9:]+"(?:\s*\|\|\s*\$name eq "[a-z_0-9:]+")*)\) \{\s*'

    # A: my scalar $args = $expr->{"args"}; gen_call_with_arg_cleanup(..., $args, N)
    for m in re.finditer(NAME + r'my scalar \$args = \$expr->\{"args"\};\s*'
                         r'gen_call_with_arg_cleanup\(\$cg, "(strada_[a-z_0-9]+)", \$args, (\d+)\);\s*'
                         r'return 1;\s*\}', src):
        add(*m.groups())

    # B: gen_call_with_arg_cleanup($cg, "strada_X", $expr->{"args"}, N)
    for m in re.finditer(NAME + r'gen_call_with_arg_cleanup\(\$cg, "(strada_[a-z_0-9]+)", '
                         r'\$expr->\{"args"\}, (\d+)\);\s*return 1;\s*\}', src):
        add(*m.groups())

    # C: zero-arg emit("strada_X()")
    for m in re.finditer(NAME + r'emit\(\$cg, "(strada_[a-z_0-9]+)\(\)"\);\s*return 1;\s*\}', src):
        add(m.group(1), m.group(2), 0)

    # D: inline emit("strada_X(") gen_expression... emit(")")
    for m in re.finditer(NAME + r'emit\(\$cg, "(strada_[a-z_0-9]+)\("\);\s*'
                         r'(?:my scalar \$args = \$expr->\{"args"\};\s*)?'
                         r'((?:gen_expression\(\$cg, \$args->\[\d+\]\);\s*(?:emit\(\$cg, ", "\);\s*)?)+)'
                         r'emit\(\$cg, "\)"\);\s*return 1;\s*\}', src):
        add(m.group(1), m.group(2), len(re.findall(r'gen_expression', m.group(3))))


def harvest_blocks(src):
    """E: inside an `if ($name eq "sys::X") {` block, every
    gen_call_with_arg_cleanup($cg, "strada_Y", $args, N) maps (X, N) -> Y
    (handlers that pick the runtime function by argument count)."""
    starts = [m for m in re.finditer(r'\n([ \t]*)if \((\$name eq "[a-z_0-9:]+"(?:\s*\|\|\s*\$name eq "[a-z_0-9:]+")*)\) \{', src)]
    for i, m in enumerate(starts):
        indent = m.group(1)
        end = src.find("\n" + indent + "}", m.end())
        if end < 0:
            continue
        body = src[m.end():end]
        for c in re.finditer(r'gen_call_with_arg_cleanup\(\$cg, "(strada_[a-z_0-9]+)", \$(?:args|expr->\{"args"\}), (\d+)\)', body):
            for n in re.findall(r'"([a-z_0-9:]+)"', m.group(2)):
                EXTRA.setdefault((n, int(c.group(2))), c.group(1))


def uniform_protos(hdr):
    protos = {}
    for m in re.finditer(r'^StradaValue\*\s+(strada_[a-z_0-9]+)\(([^)]*)\);', hdr, re.M):
        args = m.group(2).strip()
        if args in ("void", ""):
            protos[m.group(1)] = 0
        else:
            parts = [a.strip() for a in args.split(",")]
            if all(p.startswith("StradaValue *") or p.startswith("StradaValue*") for p in parts):
                protos[m.group(1)] = len(parts)
    return protos


def main():
    cgb = open("compiler/CodeGenBuiltins.strada").read()
    harvest(cgb)
    harvest_blocks(cgb)
    protos = uniform_protos(open("runtime/strada_runtime.h").read())

    # coroutines need VM-native closures (a closure cannot cross the
    # vm_to_sv bridge), so they stay compiled-only like thread::/async::
    EXCLUDE_PREFIXES = ("sys::coro_",)

    ents = sorted((n, c, a) for n, (c, a) in ENTRIES.items()
                  if (n.startswith("sys::") or n.startswith("math::"))
                  and not n.startswith(EXCLUDE_PREFIXES)
                  and a <= 4 and protos.get(c) == a)
    have_na = set((n, a) for n, _, a in ents)
    for (n, a), c in sorted(EXTRA.items()):
        if (n, a) in have_na:
            continue
        if not (n.startswith("sys::") or n.startswith("math::")) or n.startswith(EXCLUDE_PREFIXES):
            continue
        if a <= 4 and protos.get(c) == a:
            ents.append((n, c, a))
            have_na.add((n, a))
    # Keep entries of the existing table that are still valid (some were
    # added from emission patterns the harvester does not recognise).
    try:
        prev = open("interpreter/vm_generic_builtins.inc").read()
        have0 = set(n for n, _, _ in ents)
        for m in re.finditer(r'\{ "((?:sys|math)::[a-z_0-9]+)", \(void\*\)(strada_[a-z_0-9]+), (\d+) \}', prev):
            n, c, a = m.group(1), m.group(2), int(m.group(3))
            if n not in have0 and protos.get(c) == a:
                ents.append((n, c, a))
                have0.add(n)
    except OSError:
        pass
    # Bare runtime names (strada_new_undef() ...): older programs call
    # runtime functions directly and the compiled backend emits a C call.
    # Any runtime function with a uniform StradaValue* prototype qualifies.
    have = set(n for n, _, _ in ents)
    rt_src = open("runtime/strada_runtime.c").read()
    defined = set(re.findall(r'^StradaValue\s*\*\s*(strada_[a-z_0-9]+)\s*\(', rt_src, re.M))
    for c, a in protos.items():
        if c not in defined:
            continue
        if a <= 4 and c not in have and not c.startswith(("strada_coro_", "strada_thread_", "strada_async_")):
            ents.append((c, c, a))
    ents.sort()

    out = []
    out.append("/* Generated: generic runtime-bridged builtins for the VM.")
    out.append(" * Harvested from compiler/CodeGenBuiltins.strada emission patterns and")
    out.append(" * validated against strada_runtime.h (uniform StradaValue* signatures).")
    out.append(" * Regenerate with tools/gen_vm_generic_builtins.py. Names use the")
    out.append(" * normalized sys:: spelling (core:: is normalized before lookup). */")
    out.append("")
    out.append("typedef StradaValue* (*VMGenericFn0)(void);")
    out.append("typedef StradaValue* (*VMGenericFn1)(StradaValue*);")
    out.append("typedef StradaValue* (*VMGenericFn2)(StradaValue*, StradaValue*);")
    out.append("typedef StradaValue* (*VMGenericFn3)(StradaValue*, StradaValue*, StradaValue*);")
    out.append("typedef StradaValue* (*VMGenericFn4)(StradaValue*, StradaValue*, StradaValue*, StradaValue*);")
    out.append("")
    out.append("typedef struct { const char *name; void *fn; uint8_t argc; } VMGenericBuiltin;")
    out.append("")
    out.append("static const VMGenericBuiltin vm_generic_builtins[] = {")
    for n, c, a in ents:
        out.append('    { "%s", (void*)%s, %d },' % (n, c, a))
    out.append("};")
    out.append("")
    out.append("#define VM_GENERIC_BUILTIN_COUNT "
               "((int)(sizeof(vm_generic_builtins)/sizeof(vm_generic_builtins[0])))")

    open("interpreter/vm_generic_builtins.inc", "w").write("\n".join(out) + "\n")
    ns = collections.Counter(n.split("::")[0] for n, _, _ in ents)
    print("wrote %d entries: %s" % (len(ents), dict(ns)))


if __name__ == "__main__":
    main()
