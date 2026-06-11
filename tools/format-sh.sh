#!/bin/sh
# ============================================================
# format-sh.sh - shell script formatter (native Linux)
#
# Enforces the repo indentation style on .sh files: tabs for
# indentation (width 4, remainder kept as spaces) and trailing
# whitespace stripped. Code text itself is never touched.
# Windows counterpart: tools/format-sh.ps1 (keep in sync).
#
# Usage:
#   tools/format-sh.sh               format all .sh in the repo
#   tools/format-sh.sh --check       report unformatted files,
#                                    change nothing (exit code =
#                                    number of offending files)
#   tools/format-sh.sh f.sh ...      format given files
# ============================================================
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"

check=0
if [ "${1:-}" = "--check" ]; then
	check=1
	shift
fi
if [ $# -eq 0 ]; then
	set -- $(find "$root" "$root/tools" -maxdepth 1 -name '*.sh' | sort)
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
	if [ "$dirty" -eq 0 ]; then echo 'All .sh files formatted.'; fi
	exit "$dirty"
fi
