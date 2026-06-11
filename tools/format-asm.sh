#!/bin/sh
# ============================================================
# format-asm.sh - NASM source formatter (native Linux port of
# format-asm.ps1 - keep the style rules in sync!)
#
# Usage:
#   tools/format-asm.sh              format all .asm under src/
#   tools/format-asm.sh --check      report unformatted files,
#                                    change nothing (exit code =
#                                    number of offending files)
#   tools/format-asm.sh f.asm ...    format given files
#
# Style rules (whitespace and comment layout only - never touches
# code text itself):
#   - trailing whitespace stripped
#   - indented code lines use exactly one tab (displayed width 4,
#     same as the C and PowerShell convention in this repo)
#   - trailing comments aligned with spaces to visual column 32
#     (or one space after the code when it is longer)
#   - comment-only lines: column 0 stays, shallow indents (< 16)
#     become one tab, deep indents (continuations of a trailing
#     comment) become column 32
#   - original line-ending style (CRLF/LF) and the presence of a
#     final newline are preserved
# ============================================================
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"

check=0
if [ "${1:-}" = "--check" ]; then
	check=1
	shift
fi
if [ $# -eq 0 ]; then
	set -- $(find "$root/src" -name '*.asm' | sort)
fi

awk_prog='
function expand_tabs(s,    out, i, c) {
	if (index(s, "\t") == 0) return s
	out = ""
	for (i = 1; i <= length(s); i++) {
		c = substr(s, i, 1)
		if (c == "\t") {
			do { out = out " " } while (length(out) % 4)
		} else {
			out = out c
		}
	}
	return out
}
# Split s into g_code + g_comment, honoring NASM string literals
# so a quoted ";" is not mistaken for a comment start.
function split_comment(s,    i, c, quote) {
	quote = ""
	g_code = s
	g_comment = ""
	g_has = 0
	for (i = 1; i <= length(s); i++) {
		c = substr(s, i, 1)
		if (quote != "") {
			if (c == quote) quote = ""
		} else if (c == SQ || c == "\"") {
			quote = c
		} else if (c == ";") {
			g_code = substr(s, 1, i - 1)
			g_comment = substr(s, i)
			g_has = 1
			return
		}
	}
}
function spaces(n,    s) {
	s = ""
	while (n-- > 0) s = s " "
	return s
}
BEGIN { SQ = sprintf("%c", 39) }
{
	line = expand_tabs($0)
	sub(/[ \t]+$/, "", line)
	if (line == "") { printf "%s", EOL; next }

	rest = line
	sub(/^ +/, "", rest)
	indent = length(line) - length(rest)

	if (substr(rest, 1, 1) == ";") {
		# comment-only line
		if (indent == 0)      out = rest
		else if (indent < 16) out = "\t" rest
		else                  out = spaces(32) rest
		printf "%s%s", out, EOL
		next
	}

	split_comment(rest)
	code = g_code
	sub(/[ \t]+$/, "", code)
	out = (indent > 0 ? "\t" code : code)
	if (g_has) {
		# visual width: the leading tab displays as 4 columns
		width = length(out) + (substr(out, 1, 1) == "\t" ? 3 : 0)
		out = out spaces(width < 32 ? 32 - width : 1) g_comment
	}
	printf "%s%s", out, EOL
}
'

dirty=0
for f in "$@"; do
	# CRLF detection must be byte-exact (grep may strip CR on MSYS)
	if [ "$(wc -c < "$f")" -ne "$(tr -d '\r' < "$f" | wc -c)" ]; then
		eol='\r\n'; eolbytes=2
	else
		eol='\n'; eolbytes=1
	fi
	tmp="$f.fmt.$$"
	tr -d '\r' < "$f" | awk -v EOL="$eol" "$awk_prog" > "$tmp"
	if [ -n "$(tail -c 1 "$f")" ]; then
		# original had no final newline - drop the one awk added
		head -c "-$eolbytes" "$tmp" > "$tmp.nl" && mv "$tmp.nl" "$tmp"
	fi
	if cmp -s "$f" "$tmp"; then
		rm -f "$tmp"
	elif [ "$check" = 1 ]; then
		dirty=$((dirty + 1))
		echo "needs formatting: $f"
		rm -f "$tmp"
	else
		dirty=$((dirty + 1))
		cat "$tmp" > "$f"
		rm -f "$tmp"
		echo "formatted: $f"
	fi
done

if [ "$check" = 1 ]; then
	if [ "$dirty" -eq 0 ]; then echo 'All .asm files formatted.'; fi
	exit "$dirty"
fi
