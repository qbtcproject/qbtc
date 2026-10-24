param([string]$SessionFile='', [string]$RemoteOnion='', [switch]$FirstNode, [switch]$Mine)
$ErrorActionPreference='Stop'
$data=Join-Path $env:LOCALAPPDATA 'QBTC-test-v2'
$torData=Join-Path $data 'tor-trial'
$node=$null; $torProcess=$null; $bridge=$null; $sessionLock=$null
function Port-Free([int]$port) {
    $listener=New-Object Net.Sockets.TcpListener([Net.IPAddress]::Any,$port)
    $listener.ExclusiveAddressUse=$true
    try { $listener.Start(); return $true } catch { return $false } finally { $listener.Stop() }
}
function Run-Checked([string]$file,[string[]]$Arguments,[string]$label,[int]$seconds=15) {
    $out=Join-Path $torData ($label+'.stdout.txt'); $err=Join-Path $torData ($label+'.stderr.txt')
    $start=New-Object Diagnostics.ProcessStartInfo
    $start.FileName=$file
    $start.Arguments=($Arguments | ForEach-Object {'"'+([string]$_)+'"'}) -join ' '
    $start.WorkingDirectory=$torData
    $start.UseShellExecute=$false
    $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true
    $start.RedirectStandardError=$true
    $process=New-Object Diagnostics.Process
    $process.StartInfo=$start
    try {
        if (!$process.Start()) { throw ($label+': process did not start') }
        # Keep ownership of the process handle; Start-Process/HasExited on Windows
        # PowerShell 5.1 can leave ExitCode null for a fast-exiting child.
        $stdoutTask=$process.StandardOutput.ReadToEndAsync()
        $stderrTask=$process.StandardError.ReadToEndAsync()
        $until=[DateTime]::UtcNow.AddSeconds($seconds); $next=[DateTime]::UtcNow.AddSeconds(10)
        $expired=$false
        while (!$process.WaitForExit(250)) {
            if ([DateTime]::UtcNow -ge $until) { $expired=$true; $process.Kill(); break }
            if ([DateTime]::UtcNow -ge $next) { Write-Host ($label+': still connecting; bounded wait, '+[int]($until-[DateTime]::UtcNow).TotalSeconds+' seconds left...'); $next=[DateTime]::UtcNow.AddSeconds(10) }
        }
        if (!$process.WaitForExit(3000)) { throw ($label+': process did not stop') }
        if (!$stdoutTask.Wait(2000) -or !$stderrTask.Wait(2000)) { throw ($label+': output capture did not finish') }
        $stdout=$stdoutTask.Result; $stderr=$stderrTask.Result
        [IO.File]::WriteAllText($out,$stdout,(New-Object Text.UTF8Encoding($false)))
        [IO.File]::WriteAllText($err,$stderr,(New-Object Text.UTF8Encoding($false)))
        if ($expired) { throw ($label+' exceeded '+$seconds+' seconds. See '+$torData) }
        $exitCode=$process.ExitCode
        if ($null -eq $exitCode) { throw ($label+': process exit code unavailable; logs: '+$torData) }
        $result=($stdout+"`n"+$stderr).Trim()
        if ($exitCode -ne 0) { throw ($label+' exited with code '+$exitCode+': '+$result) }
        return $result
    } finally { $process.Dispose() }
}
try {
    Write-Host 'QBTC - TOR TRIAL 2 FIX 1 | automatic node connection' -ForegroundColor Cyan
    New-Item -ItemType Directory -Path $torData -Force | Out-Null
    $sessionLock=[IO.File]::Open((Join-Path $torData 'session.lock'),'OpenOrCreate','ReadWrite','None')
    foreach ($port in @(19021,19121,19022,19050)) { if (!(Port-Free $port)) { throw "Port $port is in use. Close the OLD QBTC/Tor/bridge windows and run this launcher again. No existing process was stopped." } }
    $peerExe=Join-Path $PSScriptRoot 'bin\qbtc-peer.exe'
    $info=Run-Checked (Join-Path $PSScriptRoot 'bin\qbtc.exe') @('info') 'mainnet-info'
    if ($info -notmatch 'network=mainnet') { throw 'Mainnet executables are required.' }
    $choices=@()
    foreach ($item in @(Get-NetIPConfiguration | Where-Object {$_.NetAdapter.Status -eq 'Up' -and $_.IPv4DefaultGateway})) {
        foreach ($address in $item.IPv4Address) {
            $ip=[string]$address.IPAddress; $b=([Net.IPAddress]::Parse($ip)).GetAddressBytes()
            if ($b[0] -eq 10 -or ($b[0] -eq 172 -and $b[1] -ge 16 -and $b[1] -le 31) -or ($b[0] -eq 192 -and $b[1] -eq 168)) {
                $choices+=@{ip=$ip;metric=[int](Get-NetIPInterface -InterfaceIndex $item.InterfaceIndex -AddressFamily IPv4).InterfaceMetric}
            }
        }
    }
    if (!$choices.Count) { throw 'No active private IPv4 was found.' }
    $bindAddress=($choices | Sort-Object {$_.metric} | Select-Object -First 1).ip
    $runtime=Join-Path $torData 'runtime-15.0.25'
    $torExe=Join-Path $runtime 'tor\tor.exe'
    if (!(Test-Path -LiteralPath $torExe) -and (Test-Path -LiteralPath $runtime)) {
        $found=@(Get-ChildItem -LiteralPath $runtime -Recurse -Filter tor.exe -File)
        if ($found.Count -eq 1) { $torExe=$found[0].FullName }
    }
    if (!(Test-Path -LiteralPath $torExe)) {
        Write-Host 'Downloading the official Tor Expert Bundle (about 21 MiB)...'
        $archive=Join-Path $torData 'tor-expert-15.0.25.tar.gz'
        [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
        $url='https://dist.torproject.org/torbrowser/15.0.25/tor-expert-bundle-windows-x86_64-15.0.25.tar.gz'
        Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing -TimeoutSec 180
        $expected='c1988c701de2f24f7d098d1d6d7f507b58f27740d097f29c246e0524d639b252'
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Tor archive checksum mismatch. Nothing from that download was executed.' }
        New-Item -ItemType Directory -Path $runtime -Force | Out-Null
        & tar.exe -xzf $archive -C $runtime
        if ($LASTEXITCODE -ne 0) { throw 'Tor extraction failed. Windows tar.exe is required.' }
        if (!(Test-Path -LiteralPath $torExe)) {
            $found=@(Get-ChildItem -LiteralPath $runtime -Recurse -Filter tor.exe -File)
            if ($found.Count -ne 1) { throw 'The official archive did not contain one identifiable tor.exe.' }
            $torExe=$found[0].FullName
        }
    }
    Add-Type -Path (Join-Path $PSScriptRoot 'OnionBridge.cs')
    $service=Join-Path $torData 'onion-service'
    $log=Join-Path $torData 'tor.log'
    $torError=Join-Path $torData 'tor-stderr.log'
    $config=Join-Path $torData 'torrc'
    # All paths in torrc are relative to the explicitly selected working directory.
    # Tor's Log option rejected the quoted Windows path used in trial 1.
    $lines=@('ClientOnly 1','SocksPort 127.0.0.1:19050','ControlPort 0','DataDirectory state','HiddenServiceDir onion-service','HiddenServiceVersion 3',('HiddenServicePort 19021 '+$bindAddress+':19021'),'Log notice stdout')
    [IO.File]::WriteAllLines($config,[string[]]$lines,(New-Object Text.UTF8Encoding($false)))
    Push-Location $torData
    try { $verified=Run-Checked $torExe @('--verify-config','-f',$config) 'tor-config' } finally { Pop-Location }
    $torProcess=Start-Process -FilePath $torExe -ArgumentList ('-f "'+$config+'"') -WorkingDirectory $torData -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $torError
    Write-Host 'Tor starting in background. Logs are saved automatically.'
    $deadline=[DateTime]::UtcNow.AddMinutes(3); $lastProgress=''
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($torProcess.HasExited) {
            $details=@(Get-Content $log -Tail 12 -ErrorAction SilentlyContinue)+@(Get-Content $torError -Tail 12 -ErrorAction SilentlyContinue)
            throw ('Tor stopped: '+($details -join "`n"))
        }
        $progress=@(Select-String -LiteralPath $log -Pattern 'Bootstrapped' -ErrorAction SilentlyContinue | Select-Object -Last 1)
        if ($progress.Count) {
            $text=$progress[0].Line
            if ($text -ne $lastProgress) { Write-Host $text; $lastProgress=$text }
            if ($text -match 'Bootstrapped 100%') { break }
        }
        Start-Sleep -Milliseconds 500
    }
    if ($lastProgress -notmatch 'Bootstrapped 100%') { throw ('Tor bootstrap exceeded 180 seconds. Logs: '+$torData) }
    $hostname=Join-Path $service 'hostname'
    $ownOnion=(Get-Content -LiteralPath $hostname -First 1).Trim()
    if (![QbtcOnionBridge]::ValidOnion($ownOnion)) { throw 'Invalid onion service hostname.' }
    $ownOnion | Set-Content -LiteralPath (Join-Path $data 'onion-address.txt') -Encoding ASCII
    Write-Host ('YOUR NODE: '+$ownOnion) -ForegroundColor Green
    try { Set-Clipboard -Value $ownOnion } catch { }
    if (!$FirstNode -and !$RemoteOnion) { $RemoteOnion=Read-Host 'Paste other node onion address, or Enter for first node' }
    $remote=$RemoteOnion.Trim().ToLowerInvariant()
    $peerArgs=@()
    if ($remote) {
        if (![QbtcOnionBridge]::ValidOnion($remote) -or $remote -eq $ownOnion) { throw 'A different Tor v3 onion address is required.' }
        $bridge=New-Object QbtcOnionBridge(19022,19050,$remote)
        $peerArgs=@('--onion-adapter','127.0.0.1:19022')
    }
    $arguments=@('serve',(Join-Path $data 'chain.db'),'19021','--control-port','19121','--bind',$bindAddress)+$peerArgs
    if ($Mine) {
        $address=(Get-Content -LiteralPath (Join-Path $data 'address.txt') -First 1).Trim()
        if ($address -notmatch '^qbc1[a-z0-9]+$') { throw 'Create a mainnet wallet first.' }
        $arguments+=@('--mine',$address)
    }
    $line=($arguments | ForEach-Object {'"'+([string]$_)+'"'}) -join ' '
    $node=Start-Process -FilePath $peerExe -ArgumentList $line -PassThru
    Start-Sleep -Milliseconds 800
    if ($node.HasExited) { throw 'QBTC stopped. Read its node console.' }
    $local=Run-Checked $peerExe @('status',($bindAddress+':19021')) 'local-node'
    if ($local -notmatch 'network=mainnet') { throw 'Local node did not confirm mainnet.' }
    Write-Host ('LOCAL NODE OK: '+$local) -ForegroundColor Green
    if ($SessionFile) { @{bind=$bindAddress;onion=$ownOnion;phase='node-started'} | ConvertTo-Json | Set-Content -LiteralPath $SessionFile -Encoding UTF8 }
    if ($remote) {
        Write-Host 'Checking the other QBTC node through Tor (at most 95 seconds)...'
        try {
            $remoteStatus=Run-Checked $peerExe @('status','127.0.0.1:19022','--onion-adapter') 'remote-node' 95
            if ($remoteStatus -notmatch 'network=mainnet') { throw 'Remote node did not confirm mainnet.' }
            Write-Host ('REMOTE NODE VERIFIED: '+$remoteStatus) -ForegroundColor Green
        } catch {
            Write-Host ('REMOTE NOT VERIFIED: '+$_.Exception.Message+' '+$bridge.LastError) -ForegroundColor Yellow
            Write-Host 'The local node remains running. P retries the connection.'
        }
    } else { Write-Host 'First node ready. Open PC2-CONNECT.cmd on the other PC for the prepared two-PC check.' }
    Write-Host 'Mining is OFF unless you explicitly launched with -Mine.'
    Write-Host 'Keep this window and the QBTC node console open.'
    while (!$node.HasExited -and !$torProcess.HasExited) {
        $choice=(Read-Host 'S = local status | P = peer status | Q = close Tor').Trim().ToUpperInvariant()
        if ($choice -eq 'Q') { break }
        try {
            if ($choice -eq 'S') { Write-Host (Run-Checked $peerExe @('status',($bindAddress+':19021')) 'local-node') }
            if ($choice -eq 'P') {
                if (!$bridge) { Write-Host 'First node: no outgoing peer configured.'; continue }
                Write-Host (Run-Checked $peerExe @('status','127.0.0.1:19022','--onion-adapter') 'remote-node' 95)
            }
        } catch { Write-Host ($_.Exception.Message+' '+$(if ($bridge) {$bridge.LastError})) -ForegroundColor Yellow }
    }
} catch {
    $message=$_.Exception.Message
    Write-Host $message -ForegroundColor Red
    try { $message | Set-Content -LiteralPath (Join-Path $torData 'last-error.txt') -Encoding UTF8 } catch { }
    Read-Host 'Enter to close'
} finally {
    if ($bridge) { $bridge.Dispose() }
    if ($torProcess -and !$torProcess.HasExited) { $torProcess.Kill(); $torProcess.WaitForExit() }
    if ($node -and !$node.HasExited) { Write-Host 'The QBTC node remains in its console. Ctrl+C there stops it.' }
    if ($sessionLock) { $sessionLock.Dispose() }
}
