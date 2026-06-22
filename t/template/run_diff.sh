#!/bin/bash
# Differential test harness for the Strada Template::Toolkit port.
#   1. Generate golden outputs from real Perl TT (tt_ref.pl).
#   2. Render the same corpus through the Strada engine and assert equality.
# Usage: t/template/run_diff.sh [corpus_dir]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="${1:-$ROOT/t/template/cases}"

# Perl Template::Toolkit lives in /opt/bzperl.
export PERL5LIB="/opt/bzperl/lib/perl5/x86_64-linux-thread:/opt/bzperl/lib/perl5"
BZPERL="/opt/bzperl/bin/perl"

echo "==> Generating golden outputs (Perl TT) from $DIR"
"$BZPERL" "$ROOT/t/template/tt_ref.pl" "$DIR"

echo "==> Rendering through the Strada engine and diffing"
cd "$ROOT"
strada -L ./lib -o /tmp/tt_diff_runner t/template/run.strada
exec /tmp/tt_diff_runner "$DIR"
