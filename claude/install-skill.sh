#!/bin/sh
# Install the Strada skill for AI coding assistants.
#
#   curl -fsSL https://strada-lang.github.io/claude/install.sh | sh
#   curl -fsSL https://strada-lang.github.io/claude/install.sh | sh -s -- --codex
#
# By default it installs for both Claude Code (~/.claude/skills) and OpenAI
# Codex (~/.agents/skills). Options:
#   --claude    only Claude Code
#   --codex     only Codex
#   --project   install into the current directory (.claude/skills and/or
#               .agents/skills) instead of your home directory, to commit it
#               with a project
# Running it again updates the skill. It only ever replaces a folder named
# "strada" inside those skills directories.
set -eu

URL="https://strada-lang.github.io/claude/strada-skill.tar.gz"
want_claude=1
want_codex=1
base="$HOME"
only=""

for arg in "$@"; do
    case "$arg" in
        --claude) only="${only}claude " ;;
        --codex) only="${only}codex " ;;
        --project) base="$(pwd)" ;;
        -h|--help)
            echo "usage: install.sh [--claude] [--codex] [--project]"
            echo "Installs the Strada skill for Claude Code (~/.claude/skills) and"
            echo "Codex (~/.agents/skills); --project installs into the current directory."
            exit 0 ;;
        *) echo "install.sh: unknown option: $arg" >&2; exit 2 ;;
    esac
done
if [ -n "$only" ]; then
    want_claude=0; want_codex=0
    case "$only" in *claude*) want_claude=1 ;; esac
    case "$only" in *codex*) want_codex=1 ;; esac
fi

for tool in curl tar; do
    command -v "$tool" >/dev/null 2>&1 || { echo "install.sh: $tool is required" >&2; exit 1; }
done

tmp=$(mktemp -d "${TMPDIR:-/tmp}/strada-skill.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
curl -fsSL "$URL" -o "$tmp/skill.tar.gz"
tar -xzf "$tmp/skill.tar.gz" -C "$tmp"
[ -f "$tmp/strada/SKILL.md" ] || { echo "install.sh: download did not contain strada/SKILL.md" >&2; exit 1; }

install_to() {
    dir="$1"; label="$2"
    mkdir -p "$dir"
    rm -rf "$dir/strada.new"
    cp -R "$tmp/strada" "$dir/strada.new"
    rm -rf "$dir/strada"
    mv "$dir/strada.new" "$dir/strada"
    echo "Installed for $label: $dir/strada"
}

[ "$want_claude" = 1 ] && install_to "$base/.claude/skills" "Claude Code"
[ "$want_codex" = 1 ] && install_to "$base/.agents/skills" "Codex"

echo "$(cat "$tmp/strada/VERSION" 2>/dev/null || echo 'Strada skill')"
if ! command -v strada >/dev/null 2>&1; then
    echo "Note: 'strada' is not on your PATH. The skill needs the Strada toolchain:"
    echo "  https://strada-lang.github.io/docs/quickstart.html"
fi
echo "Start a new Claude Code or Codex session to pick it up."
