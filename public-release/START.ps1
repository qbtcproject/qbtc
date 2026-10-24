$ErrorActionPreference='Stop'
$log=$null
$panelLock=$null
try {
    $folder=Join-Path $env:LOCALAPPDATA 'QBTC-test-v2'
    $old=Join-Path $env:LOCALAPPDATA 'QBTC-mainnet-v1'
    if (!(Test-Path -LiteralPath $folder) -and (Test-Path -LiteralPath $old)) {
        $active=@(Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -and $_.CommandLine.IndexOf($old,[StringComparison]::OrdinalIgnoreCase) -ge 0 })
        if($active.Count -gt 0){throw 'Close the OLD QBTC panel and its Tor/node windows, then open this new QBTC.exe again.'}
        $archive=$old+'.archived-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
        Move-Item -LiteralPath $old -Destination $archive
        $null=New-Item -ItemType Directory -Path (Join-Path $folder 'tor-trial') -Force
        foreach($part in @('runtime-15.0.25','state','onion-service')) {
            $source=Join-Path (Join-Path $archive 'tor-trial') $part
            if(Test-Path -LiteralPath $source){Copy-Item -LiteralPath $source -Destination (Join-Path $folder 'tor-trial') -Recurse}
        }
    }
    $null=New-Item -ItemType Directory -Path $folder -Force
    try{$panelLock=[IO.File]::Open((Join-Path $folder 'panel.lock'),'OpenOrCreate','ReadWrite','None')}catch{throw 'QBTC is already open for this data folder. Use the existing panel.'}
    $log=Join-Path $folder 'panel-startup.log'
    [IO.File]::WriteAllText($log,('QBTC panel starting '+[DateTime]::UtcNow.ToString('o')+"`r`n"))
    & (Join-Path $PSScriptRoot 'PANEL.ps1')
} catch {
    $message=$_.Exception.Message
    $where=$_.InvocationInfo.PositionMessage
    try { if($log){[IO.File]::AppendAllText($log,($message+"`r`n"+$where+"`r`n"))} } catch {}
    $details='QBTC could not open.'+"`r`n`r`n"+$message
    if($log){$details+="`r`n`r`nDetails: "+$log}
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $null=[Windows.Forms.MessageBox]::Show($details,'QBTC startup error')
    } catch {
        $shell=New-Object -ComObject WScript.Shell
        $null=$shell.Popup($details,0,'QBTC startup error',16)
    }
    exit 32
}

finally {if($panelLock){$panelLock.Dispose()}}
