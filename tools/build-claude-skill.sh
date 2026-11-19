#!/bin/sh
# Package the Strada skill for Claude (claude/skills/strada) in every form
# people install it from:
#
#   OUT/strada/                     the skill folder (SKILL.md, scripts/, reference/)
#   OUT/strada-skill.zip            upload in claude.ai: Settings > Capabilities > Skills
#   OUT/strada-skill.tar.gz         unpack into ~/.claude/skills/ (Claude Code)
#                                   or ~/.agents/skills/ (OpenAI Codex)
#   OUT/plugins/strada/             the same skill as a Claude Code plugin
#   OUT/marketplace.json            marketplace listing for that plugin
#
# The reference files are copied from docs/ at build time, so the skill always
# ships the current docs/STRADA_FOR_CLAUDE.md and docs/BUILTIN_FUNCTIONS.md.
#
# Usage: tools/build-claude-skill.sh [OUT]          (default build/claude-skill)
#        tools/build-claude-skill.sh --site DIR     also install into the website
#                                                   checkout DIR (claude/ and
#                                                   .claude-plugin/marketplace.json)
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/claude-skill"
SITE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --site) SITE=$2; shift 2 ;;
        *) OUT=$1; shift ;;
    esac
done

VERSION=$(date -u +%Y.%-m.%-d)
COMMIT=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
DESC="Write, compile, run, test and debug Strada programs (the Perl-inspired language that compiles to native code) with Claude."

rm -rf "$OUT"
mkdir -p "$OUT/strada/reference"
cp -R "$ROOT/claude/skills/strada/." "$OUT/strada/"
cp "$ROOT/docs/STRADA_FOR_CLAUDE.md" "$OUT/strada/reference/language.md"
cp "$ROOT/docs/BUILTIN_FUNCTIONS.md" "$OUT/strada/reference/builtins.md"
# The reference is written for the dev tree; installed users run `strada`.
# The skill serves Claude, Codex and other assistants, so the title names none.
sed -i -e 's|\./strada |strada |g' \
    -e '1s|^# Strada for Claude$|# Strada language reference for AI coding assistants|' \
    "$OUT/strada/reference/language.md"
printf 'Strada skill %s (strada-lang %s)\n' "$VERSION" "$COMMIT" > "$OUT/strada/VERSION"
chmod +x "$OUT/strada/scripts/"*.sh

(cd "$OUT" && rm -f strada-skill.zip && zip -qr strada-skill.zip strada \
    && tar -czf strada-skill.tar.gz strada)

P="$OUT/plugins/strada"
mkdir -p "$P/.claude-plugin" "$P/skills"
cp -R "$OUT/strada" "$P/skills/strada"
cat > "$P/.claude-plugin/plugin.json" <<EOF
{
  "name": "strada",
  "version": "$VERSION",
  "description": "$DESC",
  "author": { "name": "Strada" },
  "homepage": "https://strada-lang.github.io/docs/ai.html",
  "repository": "https://github.com/strada-lang/strada-lang",
  "license": "GPL-2.0",
  "keywords": ["strada", "perl", "compiler", "language"]
}
EOF
cat > "$P/README.md" <<EOF
# Strada plugin for Claude Code

This plugin adds one skill, \`strada\`, which teaches Claude to write, compile,
run, test and debug programs in Strada, the strongly typed Perl-inspired
language that compiles to C and native executables. It covers the compile/check
loop, the places where Perl habits go wrong, OOP, tests, and C interop, and it
ships the full language and builtin references for Claude to consult.

## Install

    /plugin install strada --marketplace strada-lang/strada-lang.github.io

or, on older Claude Code:

    /plugin marketplace add strada-lang/strada-lang.github.io
    /plugin install strada@strada-lang

The skill needs a Strada toolchain on the machine (\`strada\` on PATH, or a
strada-lang checkout) and a C compiler. Details:
https://strada-lang.github.io/docs/ai.html
EOF

cat > "$OUT/marketplace.json" <<EOF
{
  "name": "strada-lang",
  "owner": { "name": "Strada", "url": "https://strada-lang.github.io" },
  "description": "Claude Code plugins for the Strada programming language.",
  "plugins": [
    {
      "name": "strada",
      "source": "./claude/plugins/strada",
      "description": "$DESC",
      "version": "$VERSION"
    }
  ]
}
EOF

echo "built $OUT (skill $VERSION, strada-lang $COMMIT)"

if [ -n "$SITE" ]; then
    mkdir -p "$SITE/claude" "$SITE/.claude-plugin"
    rm -rf "$SITE/claude/strada" "$SITE/claude/plugins"
    cp -R "$OUT/strada" "$SITE/claude/strada"
    cp -R "$OUT/plugins" "$SITE/claude/plugins"
    cp "$OUT/strada-skill.zip" "$OUT/strada-skill.tar.gz" "$SITE/claude/"
    cp "$OUT/marketplace.json" "$SITE/.claude-plugin/marketplace.json"
    if [ -f "$SITE/docs/ai.html" ]; then
        sed -i "s|<code id=\"skill-version\">[^<]*</code>|<code id=\"skill-version\">$VERSION</code>|" \
            "$SITE/docs/ai.html"
    fi
    echo "installed into $SITE (claude/, .claude-plugin/marketplace.json)"
fi
