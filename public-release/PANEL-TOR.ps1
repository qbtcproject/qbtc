param([Parameter(Mandatory=$true)][string]$StateFile,[Parameter(Mandatory=$true)][string]$StopFile,[Parameter(Mandatory=$true)][int]$OwnerPid,[Parameter(Mandatory=$true)][long]$OwnerStartTicks,[string]$RemoteOnion='', [switch]$Mine,[switch]$StopOnly)
$ErrorActionPreference='Stop'
$data=Join-Path $env:LOCALAPPDATA 'QBTC-test-v2'
$torData=Join-Path $data 'tor-trial'
$node=$null; $torProcess=$null; $bridge=$null; $sessionLock=$null; $script:ownedJob=$null
$ownOnion=''; $bindAddress=''; $script:phase='starting'; $script:message='Starting Tor'; $script:failed=$false
. (Join-Path $PSScriptRoot 'TOR-PROCESS.ps1')
function Check-Cancel {
    if (Test-Path -LiteralPath $StopFile) { throw 'Panel stop requested.' }
    $owner=Get-Process -Id $OwnerPid -ErrorAction SilentlyContinue
    if (!$owner -or $owner.StartTime.ToUniversalTime().Ticks -ne $OwnerStartTicks) { throw 'Panel closed.' }
}
function Write-State([string]$phase,[string]$message) {
    $script:phase=$phase; $script:message=$message
    $record=@{phase=$phase;message=$message;bind=$bindAddress;onion=$ownOnion;peer=$RemoteOnion;mining=[bool]$Mine;time=[DateTime]::UtcNow.ToString('o')}
    # This file is UI telemetry, never a prerequisite for node liveness.
    $temp=$StateFile+'.tmp'
    try {
        [IO.File]::WriteAllText($temp,($record | ConvertTo-Json -Compress),(New-Object Text.UTF8Encoding($false)))
        for($attempt=0;$attempt -lt 4;$attempt++) {
            try {
                if ([IO.File]::Exists($StateFile)) { [IO.File]::Replace($temp,$StateFile,($StateFile+'.bak')) } else { [IO.File]::Move($temp,$StateFile) }
                return
            } catch {
                if($attempt -eq 3){throw}
                Start-Sleep -Milliseconds 50
            }
        }
    } catch {
        # Keep the last complete snapshot; the next heartbeat will retry.
        [Console]::Error.WriteLine('Panel state update deferred: '+$_.Exception.Message)
    } finally {
        try{if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)}}catch{}
    }
}
function Same-FileHash([string]$left,[string]$right) {
    if (!(Test-Path -LiteralPath $left -PathType Leaf)) { return $false }
    return (Get-FileHash -LiteralPath $left -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $right -Algorithm SHA256).Hash
}
function Stop-LegacyCoordinator {
    # Only the exact previous bundled launcher, never arbitrary PowerShell or wallet operations.
    foreach($item in @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'")) {
        if ($item.ProcessId -eq $PID -or !$item.CommandLine) { continue }
        $m=[regex]::Match($item.CommandLine,'(?i)-File\s+(?:"([^"]*TOR-TRIAL\.ps1)"|(\S*TOR-TRIAL\.ps1))')
        if (!$m.Success) { continue }
        $path=$m.Groups[1].Value; if(!$path){$path=$m.Groups[2].Value}
        if ($path.StartsWith(($env:USERPROFILE+'\Downloads\'),[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $path -PathType Leaf) -and ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq '7c47db93b29e5764a0081a6e74d3d9305002c4766a537a4635df5cad2d80eb3e')) {
            Stop-Process -Id $item.ProcessId -ErrorAction Stop
            $process=Get-Process -Id $item.ProcessId -ErrorAction SilentlyContinue
            if($process){$null=$process.WaitForExit(3000)}
        }
    }
}
function Clear-PreviousListeners {
    foreach($port in @(19021,19121,19050)) {
        $listeners=@(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue)
        foreach($listener in $listeners) {
            $item=Get-CimInstance Win32_Process -Filter ('ProcessId='+$listener.OwningProcess)
            if(!$item){continue}
            $permitted=$false
            if($port -eq 19050) {
                $prefix=[IO.Path]::GetFullPath((Join-Path $torData 'runtime-15.0.25'))+'\'
                $permitted=$item.Name -eq 'tor.exe' -and $item.ExecutablePath -and $item.CommandLine -and $item.ExecutablePath.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -and $item.CommandLine.IndexOf((Join-Path $torData 'torrc'),[StringComparison]::OrdinalIgnoreCase) -ge 0
            } else {
                $permitted=$item.Name -eq 'qbtc-peer.exe' -and $item.ExecutablePath -and $item.CommandLine -and ((Same-FileHash $item.ExecutablePath (Join-Path $PSScriptRoot 'bin\qbtc-peer.exe')) -or ((Get-FileHash -LiteralPath $item.ExecutablePath -Algorithm SHA256).Hash.ToLowerInvariant() -in @('07337f3362ee75de7b16551925b3cd83392288fc7595bc93417e9d1f2d0d8b9f','26a58dd19c22974a5ed4fe89744f7fc01c04583a7efade561e745526b5848e73','36d11b7d9e9b1ef63d83d8c48d9f0a41ab43040754fe17936117819ac4cd88bc','3870f90211d4517a9c66de23bdcd3371a653452744e6f68c27085a53af940150','596eebbef6da161836080881f2028b008899f68774f8f09f66f08d32762f2560','7cad57dae21d30d10685a231406f3e68a2d7236ff93f09ff7138d1bc5161f48a','835b9cb4c18b4393c9f328ce5a0aa577d3abc785826507369789dbaf939be437','845336ec79ba5e2f5f6c5c0d9c2fabdc485588c2c8fa7da61a3498b7dbffc4c7','949fc576cd914dfc7dbcaed5a5cbe1909911c6f9414f0f34dd7af59c6d657649','a8b6ed17dfc679d16928c471c65daf7083e8714ba3a1dae993b98531cf1833a3','aa6d46f61dbebde3489f6727fe153904681f01f9d4f4129e095af8da63d02e3c','c647a85706e637300c8dde20055a63553d035bd3d5d0a2ece067e26dc7cf495e'))) -and $item.CommandLine.IndexOf((Join-Path $data 'chain.db'),[StringComparison]::OrdinalIgnoreCase) -ge 0
            }
            if(!$permitted){throw ('Port '+$port+' belongs to a different program (PID '+$item.ProcessId+'). It was not stopped.')}
            Stop-Process -Id $item.ProcessId -ErrorAction Stop
            $process=Get-Process -Id $item.ProcessId -ErrorAction SilentlyContinue
            if($process){$null=$process.WaitForExit(3000)}
        }
    }
    foreach($port in @(19021,19121,19022,19023,19024,19026,19028,19050)){if(!(Port-Free $port)){throw ('Port '+$port+' is still in use. Close the earlier QBTC bridge window.')}}
}
try {
    New-Item -ItemType Directory -Path $torData -Force | Out-Null
    Check-Cancel
    Write-State 'starting' $(if($StopOnly){'Stopping the previous QBTC session...'}else{'Preparing your node and closing the previous QBTC session...'})
    Stop-LegacyCoordinator
    $sessionLock=[IO.File]::Open((Join-Path $torData 'session.lock'),'OpenOrCreate','ReadWrite','None')
    Clear-PreviousListeners
    if($StopOnly){Write-State 'stopped' 'Node and mining stopped.'; exit 0}
    Add-Type -LiteralPath (Join-Path $PSScriptRoot 'PanelOwnedJob.cs')
    $script:ownedJob=New-Object QbtcPanelJob
    Add-Type -LiteralPath @((Join-Path $PSScriptRoot 'OnionBridge.cs'),(Join-Path $PSScriptRoot 'OnionNetwork.cs'))
    $RemoteOnion=$RemoteOnion.Trim().ToLowerInvariant()
    if($RemoteOnion -and ![QbtcOnionBridge]::ValidOnion($RemoteOnion)){throw 'Enter a Tor v3 onion address.'}
    $peerExe=Join-Path $PSScriptRoot 'bin\qbtc-peer.exe'
    $info=Run-Checked (Join-Path $PSScriptRoot 'bin\qbtc.exe') @('info') 'mainnet-info'
    if($info -notmatch 'network=mainnet'){throw 'Mainnet executables required.'}
    # Tor forwards locally: do not expose the native node on the home LAN.
    $bindAddress='127.0.0.1'
    $runtime=Join-Path $torData 'runtime-15.0.25'
    $archive=Join-Path $torData 'tor-expert-15.0.25.tar.gz'
    $expected='c1988c701de2f24f7d098d1d6d7f507b58f27740d097f29c246e0524d639b252'
    if (!(Test-Path -LiteralPath $archive)) {
        Write-State 'starting' 'Downloading the pinned Tor runtime...'
        [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
        $download=$archive+'.download-'+[guid]::NewGuid().ToString('N')
        try {
            Get-PinnedTorArchive $download $expected
            Move-Item -LiteralPath $download -Destination $archive
        } finally {if(Test-Path -LiteralPath $download){Remove-Item -LiteralPath $download -Force -ErrorAction SilentlyContinue}}
    }
    Write-State 'starting' 'Verifying and refreshing the Tor runtime...'
    $torExe=Install-VerifiedTorRuntime $archive $runtime $expected
    Check-Cancel
    $config=Join-Path $torData 'torrc';$log=Join-Path $torData 'tor.log';$torError=Join-Path $torData 'tor-stderr.log'
    $lines=@('ClientOnly 1','SocksPort 127.0.0.1:19050','ControlPort 0','DataDirectory state','HiddenServiceDir onion-service','HiddenServiceVersion 3',('HiddenServicePort 19021 '+$bindAddress+':19021'),'HiddenServicePort 19023 127.0.0.1:19023','Log notice stdout')
    [IO.File]::WriteAllLines($config,[string[]]$lines,(New-Object Text.UTF8Encoding($false)))
    $null=Run-Checked $torExe @('--verify-config','-f',$config) 'tor-config'
    $torProcess=Start-Process -FilePath $torExe -ArgumentList ('-f "'+$config+'"') -WorkingDirectory $torData -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $torError
    $script:ownedJob.Add($torProcess)
    $deadline=[DateTime]::UtcNow.AddSeconds(180);$last=''
    while([DateTime]::UtcNow -lt $deadline) {
        Check-Cancel
        if($torProcess.HasExited){throw ('Tor stopped: '+((@(Get-Content -LiteralPath $log -Tail 8)+@(Get-Content -LiteralPath $torError -Tail 8)) -join ' '))}
        $progress=@(Select-String -LiteralPath $log -Pattern 'Bootstrapped' -ErrorAction SilentlyContinue | Select-Object -Last 1)
        if($progress.Count){
            $text=$progress[0].Line
            if($last -ne $text){Write-State 'tor-starting' $text;$last=$text}
            if($text -match 'Bootstrapped 100%'){break}
        }
        Start-Sleep -Milliseconds 250
    }
    if($last -notmatch 'Bootstrapped 100%'){throw 'Tor did not finish connecting within 180 seconds.'}
    $ownOnion=(''+(Get-Content -LiteralPath (Join-Path $torData 'onion-service\hostname') -First 1)).Trim()
    if(![QbtcOnionBridge]::ValidOnion($ownOnion)){throw 'Invalid local onion address.'}
    $ownOnion | Set-Content -LiteralPath (Join-Path $data 'onion-address.txt') -Encoding ASCII
    if($RemoteOnion -eq $ownOnion){$RemoteOnion=''}
    $arguments=@('serve',(Join-Path $data 'chain.db'),'19021','--control-port','19121','--bind',$bindAddress)
    $seedFile=Join-Path $PSScriptRoot 'ONION-SEEDS.txt'
    $seedNames=@(Get-Content -LiteralPath $seedFile | Where-Object {$_ -match '^[a-z2-7]{56}\.onion$'})
    $bridge=New-Object QbtcOnionNetwork($ownOnion,(Join-Path $data 'peers-onion-v1.txt'),19050,[string[]]$seedNames,$RemoteOnion)
    $arguments+=@('--onion-adapter','127.0.0.1:19022','127.0.0.1:19024','127.0.0.1:19026','127.0.0.1:19028')
    if($Mine){
        $address=(''+(Get-Content -LiteralPath (Join-Path $data 'address.txt') -First 1)).Trim()
        $cached=Get-Content -LiteralPath (Join-Path $data 'wallet-address.json') -Raw | ConvertFrom-Json
        $walletIdentity=(Get-FileHash -LiteralPath (Join-Path $data 'wallet.qwlt') -Algorithm SHA256).Hash
        if($cached.address -ne $address -or $cached.wallet_sha256 -ne $walletIdentity){throw 'The wallet file changed. Unlock it once before mining to bind its address.'}
        if($address -notmatch '^qbc1[a-z0-9]+$'){throw 'Create or unlock your mainnet wallet first.'}
        $modeFile=Join-Path $data 'mining-mode.txt'
        $mode='medium'
        if(Test-Path -LiteralPath $modeFile){$saved=(''+(Get-Content -LiteralPath $modeFile -Raw)).Trim();if($saved -in @('low','medium','high')){$mode=$saved}}
        $arguments+=@('--mine',$address,'--mining-mode',$mode,'--mining-mode-file',$modeFile)
    }
    foreach($name in @('node.log','node-stderr.log')) { Rotate-NodeLog (Join-Path $torData $name) }
    $line=($arguments | ForEach-Object {'"'+([string]$_)+'"'}) -join ' '
    $node=Start-Process -FilePath $peerExe -ArgumentList $line -WorkingDirectory $torData -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $torData 'node.log') -RedirectStandardError (Join-Path $torData 'node-stderr.log')
    $script:ownedJob.Add($node)
    Wait-LocalNode $node $peerExe (Join-Path $data 'chain.db.control')
    $desired=$(if($Mine){'Mining to your wallet. Tor connected.'}else{'Node online. Mining off. Tor connected.'})
    Write-State 'node-ready' $desired
    $nextHealth=[DateTime]::UtcNow; $healthFailures=0
    $nextPeer=[DateTime]::UtcNow
    while($true){
        Check-Cancel
        if($node.HasExited){throw ('Node stopped: '+((Get-Content -LiteralPath (Join-Path $torData 'node-stderr.log') -Tail 8) -join ' '))}
        if($torProcess.HasExited){throw 'Tor stopped.'}
        if([DateTime]::UtcNow -ge $nextHealth){
            if($Mine -and (Get-FileHash -LiteralPath (Join-Path $data 'wallet.qwlt') -Algorithm SHA256).Hash -ne $walletIdentity){throw 'Wallet file changed while mining. Mining stopped; unlock the new wallet before restarting.'}
            try{$null=Run-Checked $peerExe @('local-status','19121','--control-file',(Join-Path $data 'chain.db.control')) 'node-health' 3;$healthFailures=0}catch{Check-Cancel;$healthFailures++;if($healthFailures -ge 3){throw 'Local node stopped responding. See node logs; your chain has not been deleted.'}}
            $nextHealth=[DateTime]::UtcNow.AddSeconds(10)
        }
        if([DateTime]::UtcNow -ge $nextPeer){
            $known=$bridge.KnownCount; $verified=$bridge.VerifiedCount
            if($verified -gt 0){
                Write-State 'peer-verified' ($desired+' Peers: '+$verified+' reachable / '+$known+' known. Automatic discovery on.')
            }else{
                Write-State 'peer-searching' ($desired+' Searching for peers ('+$known+' known). Not connected to another node yet.')
            }
            $nextPeer=[DateTime]::UtcNow.AddSeconds(3)
        }
        Start-Sleep -Milliseconds 250
    }
}catch{
    if($_.Exception.Message -in @('Panel stop requested.','Panel closed.')){Write-State 'stopping' 'Stopping your node and Tor...'}
    else{$script:failed=$true;Write-State 'error' $_.Exception.Message}
}finally{
    if($bridge){$bridge.Dispose()}
    foreach($process in @($node,$torProcess)){
        if($process){try{if(!$process.HasExited){$process.Kill();$null=$process.WaitForExit(3000)}}catch{}}
    }
    if($script:ownedJob){$script:ownedJob.Dispose()}
    if($sessionLock){$sessionLock.Dispose()}
    if(!$script:failed){Write-State 'stopped' 'Node, mining and Tor stopped.'}
}
