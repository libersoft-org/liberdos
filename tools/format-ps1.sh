#!/bin/sh
# ============================================================
# format-ps1.sh - PowerShell source formatter (native Linux,
# no PowerShell required)
#
# Whitespace-only counterpart of format-ps1.ps1: it enforces the
# repo indentation style (tabs, width 4) and strips trailing
# whitespace, but does not reflow braces/operators - that part
# needs Invoke-Formatter and runs on machines with PowerShell.
# Files produced by format-ps1.ps1 pass this script unchanged
# and vice versa.
#
# NOTE: leading whitespace is retabbed blindly - do not indent
# here-string content with spaces (repo here-strings keep their
# content at column 0 anyway).
#
# Usage:
#   tools/format-ps1.sh              format all .ps1 in the repo
#   tools/format-ps1.sh --check      report unformatted files,
#                                    change nothing (exit code =
#                                    number of offending files)
#   tools/format-ps1.sh f.ps1 ...    format given files
# ============================================================
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"

check=0
if [ "${1:-}" = "--check" ]; then
	check=1
	shift
fi
if [ $# -eq 0 ]; then
	set -- $(find "$root" "$root/tools" -maxdepth 1 -name '*.ps1' | sort)
fi

# leading whitespace -> tabs (width 4, remainder kept as spaces),
# trailing whitespace stripped, rest of the line untouched
awk_prog='
{
	line = $0
	w = 0
	i = 1
	n = length(line)
	while (i <= n) {
		c = substr(line, i, 1)
		if (c == " ")       w++
		else if (c == "\t") w += 4 - w % 4
		else                break
		i++
	}
	rest = substr(line, i)
	sub(/[ \t]+$/, "", rest)
	if (rest == "") { printf "%s", EOL; next }
	indent = ""
	t = int(w / 4)
	while (t-- > 0) indent = indent "\t"
	t = w % 4
	while (t-- > 0) indent = indent " "
	printf "%s%s%s", indent, rest, EOL
}
'

dirty=0
for f in "$@"; do
	# CRLF detection must be byte-exact (grep may strip CR on MSYS)
	if [ "$(wc -c <"$f")" -ne "$(tr -d '\r' <"$f" | wc -c)" ]; then
		eol='\r\n'
		eolbytes=2
	else
		eol='\n'
		eolbytes=1
	fi
	tmp="$f.fmt.$$"
	tr -d '\r' <"$f" | awk -v EOL="$eol" "$awk_prog" >"$tmp"
	if [ -n "$(tail -c 1 "$f")" ]; then
		# original had no final newline - drop the one awk added
		head -c "-$eolbytes" "$tmp" >"$tmp.nl" && mv "$tmp.nl" "$tmp"
	fi
	if cmp -s "$f" "$tmp"; then
		rm -f "$tmp"
	elif [ "$check" = 1 ]; then
		dirty=$((dirty + 1))
		echo "needs formatting: $f"
		rm -f "$tmp"
	else
		dirty=$((dirty + 1))
		cat "$tmp" >"$f"
		rm -f "$tmp"
		echo "formatted: $f"
	fi
done

if [ "$check" = 1 ]; then
	if [ "$dirty" -eq 0 ]; then echo 'All .ps1 files formatted.'; fi
	exit "$dirty"
fi
