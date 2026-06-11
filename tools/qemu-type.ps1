# Send keystrokes to QEMU via the TCP monitor (sendkey), then Enter.
# Usage: .\qemu-type.ps1 -Text "dir" [-Port 45454]
param(
	[Parameter(Mandatory)][string]$Text,
	[int]$Port = 45454,
	[int]$DelayMs = 50
)

$map = @{
	' ' = 'spc'; '.' = 'dot'; '-' = 'minus'; '=' = 'equal'
	'\' = 'backslash'; '/' = 'slash'; ',' = 'comma'; ';' = 'semicolon'
	"'" = 'apostrophe'; '[' = 'bracket_left'; ']' = 'bracket_right'
	'`' = 'grave_accent'
}
$shifted = @{
	'*' = 'shift-8'; '?' = 'shift-slash'; ':' = 'shift-semicolon'
	'!' = 'shift-1'; '@' = 'shift-2'; '#' = 'shift-3'; '$' = 'shift-4'
	'%' = 'shift-5'; '^' = 'shift-6'; '&' = 'shift-7'; '(' = 'shift-9'
	')' = 'shift-0'; '_' = 'shift-minus'; '+' = 'shift-equal'
	'"' = 'shift-apostrophe'; '<' = 'shift-comma'; '>' = 'shift-dot'
	'|' = 'shift-backslash'
}

$client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port)
$stream = $client.GetStream()
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true
Start-Sleep -Milliseconds 200

foreach ($ch in $Text.ToCharArray()) {
	$c = [string]$ch
	if ($map.ContainsKey($c)) { $key = $map[$c] }
	elseif ($shifted.ContainsKey($c)) { $key = $shifted[$c] }
	elseif ($c -cmatch '[A-Z]') { $key = "shift-$($c.ToLower())" }
	else { $key = $c }
	$writer.WriteLine("sendkey $key")
	Start-Sleep -Milliseconds $DelayMs
}
$writer.WriteLine('sendkey ret')
Start-Sleep -Milliseconds 200
$client.Close()
