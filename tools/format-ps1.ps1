# ============================================================
# format-ps1.ps1 - PowerShell source formatter
#
# Wraps PSScriptAnalyzer's Invoke-Formatter with the repo style
# (tabs for indentation, width 4 - same as .c/.h and .asm).
# This is the canonical .ps1 formatter; tools/format-ps1.sh is a
# whitespace-only fallback for machines without PowerShell.
#
# Usage:
#   .\tools\format-ps1.ps1            format all .ps1 in the repo
#   .\tools\format-ps1.ps1 -Check     report unformatted files,
#                                     change nothing (exit code =
#                                     number of offending files)
#   .\tools\format-ps1.ps1 -Path f.ps1 ...   format given files
#
# Requires: Install-Module PSScriptAnalyzer -Scope CurrentUser
# Original line endings and final-newline presence are kept.
# ============================================================
param(
	[switch]$Check,
	[string[]]$Path
)
$ErrorActionPreference = 'Stop'

Import-Module PSScriptAnalyzer

$settings = @{
	IncludeRules = @(
		'PSPlaceOpenBrace'
		'PSPlaceCloseBrace'
		'PSUseConsistentIndentation'
		'PSUseConsistentWhitespace'
	)
	Rules        = @{
		PSPlaceOpenBrace           = @{
			Enable             = $true
			OnSameLine         = $true
			NewLineAfter       = $true
			IgnoreOneLineBlock = $true
		}
		PSPlaceCloseBrace          = @{
			Enable             = $true
			NewLineAfter       = $false
			IgnoreOneLineBlock = $true
			NoEmptyLineBefore  = $false
		}
		PSUseConsistentIndentation = @{
			Enable              = $true
			IndentationSize     = 4
			Kind                = 'tab'
			PipelineIndentation = 'IncreaseIndentationForFirstPipeline'
		}
		PSUseConsistentWhitespace  = @{
			Enable          = $true
			CheckInnerBrace = $true
			CheckOpenBrace  = $true
			CheckOpenParen  = $true
			CheckOperator   = $false # keep manual alignment columns
			CheckPipe       = $true
			CheckSeparator  = $true
		}
	}
}

if (-not $Path) {
	$root = Split-Path $PSScriptRoot -Parent
	$Path = Get-ChildItem $root, (Join-Path $root 'tools') -Filter *.ps1 |
		ForEach-Object { $_.FullName }
}

$dirty = 0
foreach ($file in $Path) {
	$file = (Resolve-Path $file).Path
	$text = [System.IO.File]::ReadAllText($file)
	$eol  = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
	$finalNewline = $text.EndsWith("`n")

	# Invoke-Formatter works with LF internally; normalize, format,
	# strip trailing whitespace, then restore the original EOL.
	$work = $text -replace "`r`n", "`n"
	$new  = Invoke-Formatter -ScriptDefinition $work -Settings $settings
	$new  = (($new -split "`n") | ForEach-Object { $_.TrimEnd() }) -join "`n"
	$new  = $new.TrimEnd("`n") + $(if ($finalNewline) { "`n" } else { '' })
	if ($eol -ne "`n") { $new = $new -replace "`n", $eol }

	if ($new -ne $text) {
		$dirty++
		if ($Check) {
			Write-Host "needs formatting: $file" -ForegroundColor Yellow
		} else {
			[System.IO.File]::WriteAllText($file, $new,
				[System.Text.Encoding]::ASCII)
			Write-Host "formatted: $file"
		}
	}
}

if ($Check) {
	if ($dirty -eq 0) { Write-Host 'All .ps1 files formatted.' -ForegroundColor Green }
	exit $dirty
}
