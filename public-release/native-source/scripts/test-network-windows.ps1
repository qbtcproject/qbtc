# Windows PowerShell 5.1+. Uses fresh temporary regtest data, not the user's chain.
$ErrorActionPreference = 'Stop'
$binary = Join-Path $PSScriptRoot 'bin\qbtc-peer.exe'
if (!(Test-Path $binary)) { $binary = Join-Path $PSScriptRoot '..\build-windows\qbtc-peer.exe' }
$binary = (Resolve-Path $binary).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ('qbtc-auto-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$script:Nodes = @($null, $null, $null)
$ports = New-Object 'System.Collections.Generic.List[int]'
while ($ports.Count -lt 6) {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $candidate = $listener.LocalEndpoint.Port
    $listener.Stop()
    if (!$ports.Contains($candidate)) { $ports.Add($candidate) }
}
function Get-Status([int]$index) {
    $result = & $binary status $ports[$index] 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Status failed: $result" }
    return ($result -join ' ')
}
function Get-Field([string]$text, [string]$name) {
    if ($text -notmatch ('(?:^|\s)' + [Regex]::Escape($name) + '=([^\s]+)')) { throw "Missing field $name" }
    return $Matches[1]
}
function Wait-For([scriptblock]$condition) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 45) {
        try { if (& $condition) { return } } catch { }
        Start-Sleep -Milliseconds 100
    }
    throw 'Network test condition timed out.'
}
function Start-Node([int]$index, [int[]]$seeds) {
    $chain = Join-Path $work "$index.db"
    $arguments = @('serve', ('"' + $chain + '"'), $ports[$index], '--control-port', $ports[$index+3], '--discover')
    foreach ($seed in $seeds) { $arguments += "127.0.0.1:$($ports[$seed])" }
    $token = [Guid]::NewGuid().ToString('N')
    $script:Nodes[$index] = Start-Process -FilePath $binary -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $work "$token.out") -RedirectStandardError (Join-Path $work "$token.err")
    Wait-For { Get-Status $index }
}
function Stop-Node([int]$index) {
    $process = $script:Nodes[$index]
    if ($null -ne $process) {
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
        $script:Nodes[$index] = $null
    }
}
function Mine-Two([int]$index) {
    $address = 'qbr1pqumxsfhvtyw2n4gxlmkfw3nxtqyvxs0hsaytc9zxkdw0r6gc78sqpl53r3'
    $control = Join-Path $work "$index.db.control"
    $result = & $binary mine $ports[$index+3] $address 2 --control-file $control 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Mining failed: $result" }
}
$code = 0
try {
    Start-Node 2 @()
    Start-Node 1 @(2)
    Start-Node 0 @(1)
    Wait-For {
        ((Get-Field (Get-Status 0) 'peer_verified') -eq '2') -and
        ((Get-Field (Get-Status 1) 'peer_verified') -eq '2') -and
        ((Get-Field (Get-Status 2) 'peer_verified') -eq '2')
    }
    Write-Output 'PASS Windows automatic discovery: three independent node processes'
    Stop-Node 1
    Mine-Two 0
    Wait-For { (Get-Field (Get-Status 2) 'height') -eq '2' }
    if ((Get-Field (Get-Status 0) 'tip') -ne (Get-Field (Get-Status 2) 'tip')) { throw 'Tip mismatch' }
    if ((Get-Field (Get-Status 0) 'utxo_hash') -ne (Get-Field (Get-Status 2) 'utxo_hash')) { throw 'UTXO mismatch' }
    Write-Output 'PASS Windows synchronization after removing the initial bootstrap node'
    Stop-Node 0
    Start-Node 0 @()
    Mine-Two 2
    Wait-For { (Get-Field (Get-Status 0) 'height') -eq '4' }
    if ((Get-Field (Get-Status 0) 'tip') -ne (Get-Field (Get-Status 2) 'tip')) { throw 'Restart tip mismatch' }
    if ((Get-Field (Get-Status 0) 'utxo_hash') -ne (Get-Field (Get-Status 2) 'utxo_hash')) { throw 'Restart UTXO mismatch' }
    Write-Output 'PASS Windows peer persistence: reconnect after restart without seed arguments'
    Write-Output 'PASS automatic local TCP network campaign; real Internet/NAT remains untested'
} catch {
    Write-Output "FAIL Windows automatic network test: $_"
    foreach ($path in Get-ChildItem -LiteralPath $work -Filter '*.err') {
        Get-Content -LiteralPath $path.FullName | Write-Output
    }
    $code = 1
} finally {
    for ($index=0; $index -lt 3; $index++) { Stop-Node $index }
    Remove-Item -LiteralPath $work -Recurse -Force
}
exit $code
