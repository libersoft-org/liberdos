# ============================================================
# format-sh.ps1 - shell script formatter (Windows counterpart
# of tools/format-sh.sh - keep the style rules in sync!)
#
# Enforces the repo indentation style on .sh files: tabs for
# indentation (width 4, remainder kept as spaces) and trailing
# whitespace stripped. Code text itself is never touched.
#
# Usage:
#   .\tools\format-sh.ps1             format all .sh in the repo
#   .\tools\format-sh.ps1 -Check      report unformatted files,
#                                     change nothing (exit code =
#                                     number of offending files)
#   .\tools\format-sh.ps1 -Path f.sh ...   format given files
# ============================================================
param(
	[switch]$Check,
	[string[]]$Path
)
$ErrorActionPreference = 'Stop'

$TAB_WIDTH = 4

function Format-Line([string]$line) {
	# visual width of the leading whitespace
	$w = 0
	$i = 0
	while ($i -lt $line.Length) {
		$ch = $line[$i]
		if ($ch -eq ' ') { $w++ }
		elseif ($ch -eq "`t") { $w += $TAB_WIDTH - ($w % $TAB_WIDTH) }
		else { break }
		$i++
	}
	$rest = $line.Substring($i).TrimEnd()
	if (-not $rest) { return '' }
	return ("`t" * [int][math]::Floor($w / $TAB_WIDTH)) +
	(' ' * ($w % $TAB_WIDTH)) + $rest
}

if (-not $Path) {
	$root = Split-Path $PSScriptRoot -Parent
	$Path = Get-ChildItem $root, (Join-Path $root 'tools') -Filter *.sh |
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
	if ($dirty -eq 0) { Write-Host 'All .sh files formatted.' -ForegroundColor Green }
	exit $dirty
}
