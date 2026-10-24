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
            Check-Cancel
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
    } finally {
        try { if (!$process.HasExited) { $process.Kill(); $null=$process.WaitForExit(3000) } } catch { }
        $process.Dispose()
    }
}

function Install-VerifiedTorRuntime([string]$Archive,[string]$Runtime,[string]$ExpectedHash) {
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedHash) {
        throw 'Tor archive checksum mismatch. No runtime from this archive was executed.'
    }
    $stage=$Runtime+'.new-'+[guid]::NewGuid().ToString('N')
    $previous=$Runtime+'.old-'+[guid]::NewGuid().ToString('N')
    $tar=Join-Path $env:SystemRoot 'System32\tar.exe'
    $installed=$false
    try {
        $null=New-Item -ItemType Directory -Path $stage
        & $tar -xzf $Archive -C $stage
        if ($LASTEXITCODE -ne 0) { throw 'Verified Tor archive extraction failed.' }
        $found=@(Get-ChildItem -LiteralPath $stage -Recurse -Filter tor.exe -File)
        if($found.Count -ne 1){throw 'Verified Tor archive must contain exactly one tor.exe.'}
        $relative=$found[0].FullName.Substring($stage.Length+1)
        if(Test-Path -LiteralPath $Runtime){Move-Item -LiteralPath $Runtime -Destination $previous}
        try {Move-Item -LiteralPath $stage -Destination $Runtime;$installed=$true}
        catch {if(Test-Path -LiteralPath $previous){Move-Item -LiteralPath $previous -Destination $Runtime};throw}
        return Join-Path $Runtime $relative
    } finally {
        if(Test-Path -LiteralPath $stage){Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue}
        if($installed -and (Test-Path -LiteralPath $previous)){Remove-Item -LiteralPath $previous -Recurse -Force -ErrorAction SilentlyContinue}
    }
}

function Wait-LocalNode($Node,[string]$PeerExe,[string]$ControlFile,[int]$Seconds=120) {
    $deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
    $lastError='No authenticated response yet.'
    Write-State 'node-starting' 'Opening and validating your chain. This may take longer after an interrupted shutdown...'
    while([DateTime]::UtcNow -lt $deadline) {
        Check-Cancel
        $Node.Refresh()
        if($Node.HasExited){throw ('Node stopped during startup. See node-stderr.log. '+$lastError)}
        try {
            $remaining=[Math]::Max(1,[Math]::Min(3,[int]($deadline-[DateTime]::UtcNow).TotalSeconds))
            $local=Run-Checked $PeerExe @('local-status','19121','--control-file',$ControlFile) 'local-node' $remaining
            if($local -match '^network=mainnet\s'){return}
            $lastError='Unexpected local network response.'
        }catch{
            Check-Cancel
            $lastError=$_.Exception.Message
        }
        Start-Sleep -Milliseconds 250
    }
    throw ('Local node did not become ready within '+$Seconds+' seconds. See node.log and node-stderr.log. '+$lastError)
}

function Rotate-NodeLog([string]$Path) {
    # Keep one bounded previous-session log instead of overwriting the only evidence.
    if(Test-Path -LiteralPath $Path){Move-Item -LiteralPath $Path -Destination ($Path+'.previous') -Force}
}

function Get-PinnedTorArchive([string]$Destination,[string]$ExpectedHash) {
    $urls=@(
        'https://dist.torproject.org/torbrowser/15.0.25/tor-expert-bundle-windows-x86_64-15.0.25.tar.gz',
        'https://archive.torproject.org/tor-package-archive/torbrowser/15.0.25/tor-expert-bundle-windows-x86_64-15.0.25.tar.gz'
    )
    $previousProgress=$ProgressPreference
    $ProgressPreference='SilentlyContinue'
    try {
        $attempt=0
        foreach($url in $urls){
            $attempt++
            Check-Cancel
            $client=New-Object Net.WebClient
            try {
                Write-State 'tor-downloading' ('Downloading verified Tor runtime (source '+$attempt+'/2)...')
                $task=$client.DownloadFileTaskAsync([Uri]$url,$Destination)
                $deadline=[DateTime]::UtcNow.AddSeconds(600)
                $next=[DateTime]::UtcNow
                while(!$task.IsCompleted){
                    Check-Cancel
                    if([DateTime]::UtcNow -ge $deadline){throw 'Tor download timed out after 600 seconds.'}
                    if([DateTime]::UtcNow -ge $next){
                        $size=0
                        if(Test-Path -LiteralPath $Destination){$size=(Get-Item -LiteralPath $Destination).Length}
                        Write-State 'tor-downloading' ('Downloading Tor: '+[Math]::Round($size/1MB,1)+' MB received (source '+$attempt+'/2). You can stop the node to cancel.')
                        $next=[DateTime]::UtcNow.AddSeconds(1)
                    }
                    Start-Sleep -Milliseconds 250
                }
                $task.GetAwaiter().GetResult()
            }catch{
                Check-Cancel
                if($attempt -eq $urls.Count){throw ('Tor download failed from both official sources: '+$_.Exception.Message)}
                continue
            }finally{$client.CancelAsync();$client.Dispose()}
            # Both sources must deliver the identical pinned archive, never a substituted version.
            if((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedHash){throw 'Tor download checksum mismatch. Nothing was installed.'}
            return
        }
    }finally{$ProgressPreference=$previousProgress}
}
