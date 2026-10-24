# Execute the real persistence/read functions without starting the GUI or a node.
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
foreach($entry in @(@('PANEL-TOR.ps1','Write-State'),@('PANEL.ps1','Read-PanelSession'))){
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root $entry[0]),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw $errors[0]}
    $name=$entry[1]
    $definition=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true)
    . ([scriptblock]::Create($definition.Extent.Text))
}
$folder=Join-Path ([IO.Path]::GetTempPath()) ('qbtc-state-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($folder)
try{
    $StateFile=Join-Path $folder 'state.json';$bindAddress='127.0.0.1';$ownOnion='';$RemoteOnion='';$Mine=$true
    Write-State 'node-ready' 'first'
    if((Read-PanelSession $StateFile).message -ne 'first'){throw 'Initial snapshot missing'}
    # Windows must permit replacement while a reader still owns the old file.
    $held=[IO.File]::Open($StateFile,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try{Write-State 'peer-verified' 'second'}finally{$held.Dispose()}
    if((Read-PanelSession $StateFile).message -ne 'second'){throw 'Shared reader blocked heartbeat'}
    if($IsWindows -or $env:OS -eq 'Windows_NT'){
        $held=[IO.File]::Open($StateFile,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
        try{Write-State 'peer-verified' 'locked'}finally{$held.Dispose()}
        if((Read-PanelSession $StateFile).message -ne 'second'){throw 'Locked snapshot altered'}
    }
    # Persistent write failure is non-fatal, including catch/finally status paths.
    $good=$StateFile;$StateFile=Join-Path $folder 'blocked';$null=[IO.Directory]::CreateDirectory($StateFile)
    Write-State 'peer-verified' 'blocked'
    if(Test-Path -LiteralPath ($StateFile+'.tmp')){throw 'Temporary file leaked'}
    $StateFile=$good
    Write-State 'peer-verified' 'recovered'
    if((Read-PanelSession $StateFile).message -ne 'recovered'){throw 'Heartbeat did not recover'}
    1..100 | ForEach-Object {Write-State 'peer-verified' "heartbeat $_";$state=Read-PanelSession $StateFile;if($state.message -ne "heartbeat $_"){throw 'Incomplete snapshot'}}
    'PASS state read sharing, replacement, non-fatal storage error, cleanup, recovery and 100 heartbeats'
}finally{Remove-Item -LiteralPath $folder -Recurse -Force}
