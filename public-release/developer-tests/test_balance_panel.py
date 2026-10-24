"""Real PowerShell refresh/timer/display functions, native read-only CLI; GUI controls mocked."""
import os
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = pathlib.Path(os.environ.get('QBTC_NATIVE_BUILD', ROOT / 'native-source/build-mainnet'))
PWSH = os.environ.get('QBTC_PWSH', '/tmp/qbtc-pwsh/pwsh')
with tempfile.TemporaryDirectory(prefix='qbtc-balance-panel-') as temporary:
    folder = pathlib.Path(temporary)
    addresses = subprocess.check_output([BUILD / 'balance-fixture', folder], text=True).splitlines()
    (folder / 'bin').mkdir()
    (folder / 'bin/qbtc-wallet.exe').symlink_to((BUILD / 'qbtc-wallet').resolve())
    shutil.copy2(folder / 'mainnet.db', folder / 'chain.db')
    script = folder / 'check.ps1'
    script.write_text(r'''
$ErrorActionPreference='Stop'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($env:QBTC_PANEL,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Panel syntax'}
foreach($name in @('Quote-Arg','To-Coins','Balance-Display','Refresh-Balance')){
    $definition=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true)
    . ([scriptblock]::Create($definition.Extent.Text.Replace('$PSScriptRoot','$env:QBTC_BALANCE_ROOT')))
}
$poll=$ast.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Extent.Text.StartsWith('if($script:balancePoll){') -and $n.Extent.Text.Contains('StandardOutput.ReadToEnd')},$true)
if(!$poll){throw 'Missing balance completion handler'}
$complete=[scriptblock]::Create($poll.Extent.Text)
function Tell([string]$message){throw $message}
$PSScriptRoot=$env:QBTC_BALANCE_ROOT
$data=$env:QBTC_BALANCE_ROOT
$script:address=$env:QBTC_PUBLIC_ADDRESS
$script:hiddenBalance=$true
$script:balanceValue='--';$script:spendableValue='--';$script:balanceTime='Not loaded'
$script:balancePoll=$null
$refresh=[pscustomobject]@{Enabled=$true}
$notice=[pscustomobject]@{Text=''}
$balance=[pscustomobject]@{Text=''};$available=[pscustomobject]@{Text=''};$balanceStamp=[pscustomobject]@{Text=''}
$eye=[pscustomobject]@{AccessibleName=''};$eye | Add-Member ScriptMethod Invalidate {}
Refresh-Balance
if(!$script:balancePoll -or $refresh.Enabled){throw 'Refresh did not start asynchronously'}
$deadline=[DateTime]::UtcNow.AddSeconds(10)
while($script:balancePoll -and [DateTime]::UtcNow -lt $deadline){. $complete;Start-Sleep -Milliseconds 20}
if($script:balancePoll -or !$refresh.Enabled){throw 'Refresh did not finish'}
if($script:balanceValue -ne '0.00000000' -or $script:spendableValue -ne '0.00000000'){throw $notice.Text}
if($balance.Text -ne '******** QBTC'){throw 'Refresh revealed hidden balance'}
$script:hiddenBalance=$false;Balance-Display
if($balance.Text -ne '0.00000000 QBTC'){throw 'Eye did not reveal cached public balance'}
$script:hiddenBalance=$true;Balance-Display
if($balance.Text -ne '******** QBTC'){throw 'Eye did not hide balance'}
$script:balanceValue='sentinel'
Refresh-Balance
$script:address='changed while reading'
$deadline=[DateTime]::UtcNow.AddSeconds(10)
while($script:balancePoll -and [DateTime]::UtcNow -lt $deadline){. $complete;Start-Sleep -Milliseconds 20}
if($script:balanceValue -ne 'sentinel'){throw 'Stale address result was displayed'}
Write-Output 'PASS real asynchronous balance query; password-free with no wallet file; hidden/revealed display; changed-address result discarded'
''')
    env = dict(os.environ, QBTC_PANEL=str(ROOT / 'PANEL.ps1'), QBTC_BALANCE_ROOT=str(folder), QBTC_PUBLIC_ADDRESS=addresses[2])
    subprocess.run([PWSH, '-NoProfile', '-File', script], env=env, check=True, timeout=25)
    assert not list(folder.glob('*.qwlt'))
