# ============================================================
# format-asm.ps1 - NASM source formatter
# (native Linux counterpart: tools/format-asm.sh - keep in sync!)
#
# Usage:
#   .\tools\format-asm.ps1            format all .asm under src\
#   .\tools\format-asm.ps1 -Check     report unformatted files,
#                                     change nothing (exit code =
#                                     number of offending files)
#   .\tools\format-asm.ps1 -Path f.asm ...   format given files
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
param(
	[switch]$Check,
	[string[]]$Path
)
$ErrorActionPreference = 'Stop'

$COMMENT_COL = 32
$TAB_WIDTH   = 4

# Split a line into code and trailing comment, honoring NASM
# string literals ('...' and "...") so a quoted ';' is not
# mistaken for a comment start. Returns @(code, commentOrNull).
function Split-Comment([string]$s) {
	$quote = ''
	for ($i = 0; $i -lt $s.Length; $i++) {
		$ch = $s[$i]
		if ($quote) {
			if ($ch -eq $quote) { $quote = '' }
		} elseif ($ch -eq "'" -or $ch -eq '"') {
			$quote = $ch
		} elseif ($ch -eq ';') {
			return @($s.Substring(0, $i), $s.Substring($i))
		}
	}
	return @($s, $null)
}

function Expand-Tabs([string]$s) {
	if ($s.IndexOf("`t") -lt 0) { return $s }
	$out = New-Object System.Text.StringBuilder
	foreach ($ch in $s.ToCharArray()) {
		if ($ch -eq "`t") {
			do { [void]$out.Append(' ') } while ($out.Length % $TAB_WIDTH)
		} else {
			[void]$out.Append($ch)
		}
	}
	return $out.ToString()
}

function Format-Line([string]$line) {
	$line = (Expand-Tabs $line).TrimEnd()
	if (-not $line) { return '' }

	$indentLen = $line.Length - $line.TrimStart().Length
	$rest      = $line.TrimStart()

	if ($rest.StartsWith(';')) {
		# comment-only line
		if ($indentLen -eq 0) { return $rest }
		if ($indentLen -lt 16) { return "`t" + $rest }
		return (' ' * $COMMENT_COL) + $rest
	}

	$code, $comment = Split-Comment $rest
	$code = $code.TrimEnd()
	$out  = if ($indentLen -gt 0) { "`t" + $code } else { $code }
	if ($null -ne $comment) {
		# visual width: the leading tab displays as TAB_WIDTH columns
		$width = $out.Length + $(if ($out[0] -eq "`t") { $TAB_WIDTH - 1 } else { 0 })
		if ($width -lt $COMMENT_COL) {
			$out += ' ' * ($COMMENT_COL - $width)
		} else {
			$out += ' '
		}
		$out += $comment
	}
	return $out
}

if (-not $Path) {
	$root = Split-Path $PSScriptRoot -Parent
	$Path = Get-ChildItem -Recurse -Filter *.asm (Join-Path $root 'src') |
		ForEach-Object { $_.FullName }
}

$dirty = 0
foreach ($file in $Path) {
	$file = (Resolve-Path $file).Path
	$text = [System.IO.File]::ReadAllText($file)
	$eol  = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
	$finalNewline = $text.EndsWith("`n")

	$lines     = $text -split "`r?`n"
	if ($finalNewline) { $lines = $lines[0..($lines.Length - 2)] }
	$formatted = $lines | ForEach-Object { Format-Line $_ }
	$newText   = ($formatted -join $eol) + $(if ($finalNewline) { $eol } else { '' })

	if ($newText -ne $text) {
		$dirty++
		if ($Check) {
			Write-Host "needs formatting: $file" -ForegroundColor Yellow
		} else {
			[System.IO.File]::WriteAllText($file, $newText,
				[System.Text.Encoding]::ASCII)
			Write-Host "formatted: $file"
		}
	}
}

if ($Check) {
	if ($dirty -eq 0) { Write-Host 'All .asm files formatted.' -ForegroundColor Green }
	exit $dirty
}
