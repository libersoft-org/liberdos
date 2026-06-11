@echo off
rem ============================================================
rem format-file.cmd - on-save formatter dispatcher (Windows)
rem
rem Counterpart of the extensionless "format-file" sh script:
rem VS Code (Run on Save) invokes "tools/format-file <file>" and
rem cmd.exe resolves it to this .cmd via PATHEXT.
rem ============================================================
if "%~1"=="" (
	echo usage: format-file ^<file^> 1>&2
	exit /b 1
)
if /i "%~x1"==".asm" (pwsh -NoProfile -File "%~dp0format-asm.ps1" -Path "%~1" & exit /b)
if /i "%~x1"==".ps1" (pwsh -NoProfile -File "%~dp0format-ps1.ps1" -Path "%~1" & exit /b)
if /i "%~x1"==".sh"  (pwsh -NoProfile -File "%~dp0format-sh.ps1"  -Path "%~1" & exit /b)
echo format-file: no formatter for %1 1>&2
exit /b 1
