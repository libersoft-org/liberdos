#!/bin/sh
# ============================================================
# format-c.sh - C source formatter (native Linux)
#
# Wrapper around clang-format with the repo .clang-format style.
# Looks for clang-format on PATH first, then falls back to the
# copy bundled with the VS Code cpptools extension.
#
# Usage:
#   tools/format-c.sh                format all .c/.h under src/
#   tools/format-c.sh --check        report unformatted files,
#                                    change nothing (exit code =
#                                    number of offending files)
#   tools/format-c.sh f.c ...        format given files
# ============================================================
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"

check=0
if [ "${1:-}" = "--check" ]; then
	check=1
	shift
fi
if [ $# -eq 0 ]; then
	set -- $(find "$root/src" -name '*.c' -o -name '*.h' | sort)
fi

cf="$(command -v clang-format 2>/dev/null || true)"
if [ -z "$cf" ]; then
	# VS Code cpptools bundles one (covers .vscode-server too)
	cf="$(ls "$HOME"/.vscode*/extensions/ms-vscode.cpptools-*/LLVM/bin/clang-format 2>/dev/null | head -n 1 || true)"
fi
if [ -z "$cf" ]; then
	echo 'clang-format not found (install it or the VS Code cpptools extension)' >&2
	exit 1
fi

dirty=0
for f in "$@"; do
	if "$cf" --dry-run -Werror "$f" 2>/dev/null; then
		continue
	fi
	dirty=$((dirty + 1))
	if [ "$check" = 1 ]; then
		echo "needs formatting: $f"
	else
		"$cf" -i "$f"
		echo "formatted: $f"
	fi
done

if [ "$check" = 1 ]; then
	if [ "$dirty" -eq 0 ]; then echo 'All .c/.h files formatted.'; fi
	exit "$dirty"
fi
