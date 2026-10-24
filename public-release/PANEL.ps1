param([string]$TorPeer='')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms.DataVisualization
[Windows.Forms.Application]::EnableVisualStyles()
$script:data = Join-Path $env:LOCALAPPDATA 'QBTC-test-v2'
New-Item -ItemType Directory -Path $data -Force | Out-Null
$script:powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$script:peer = Join-Path $PSScriptRoot 'bin\qbtc-peer.exe'
$profileInfo = @(& (Join-Path $PSScriptRoot 'bin\qbtc.exe') info)
if ($LASTEXITCODE -ne 0 -or ($profileInfo -join ' ') -notmatch 'network=mainnet') {
    [Windows.Forms.MessageBox]::Show('This panel requires the QBTC mainnet programs from the complete ZIP.','QBTC') | Out-Null
    exit 1
}
$script:address = ''
$script:hiddenBalance = $true
$script:balanceValue = '--'
$script:spendableValue = '--'
$script:balanceTime = 'Not loaded'
$script:task = $null
$script:poll = $null
$script:balancePoll = $null
$script:node = $null
$script:lanTask = $null
$script:publicTask = $null
$script:torTask = $null
$script:pendingTorStart = $null
$script:torConfigPath=Join-Path $data 'panel-tor.json'
$script:lastHeight = $null
$script:online = $false
$script:nextStatusPoll=[DateTime]::MinValue
$script:samples = 0
$script:accent = [Drawing.ColorTranslator]::FromHtml('#c77908')
$script:ink = [Drawing.ColorTranslator]::FromHtml('#18243b')
function Quote-Arg([string]$value) {
    if ($value.Contains('"') -or $value.Contains("`r") -or $value.Contains("`n")) { throw 'Invalid character in argument.' }
    return '"' + $value + '"'
}
function Tell([string]$message) { [Windows.Forms.MessageBox]::Show($message,'QBTC') | Out-Null }
function Read-Address {
    $previousAddress=$script:address
    $script:address = ''
    $path = Join-Path $data 'address.txt'
    if (Test-Path -LiteralPath $path) {
        $value = (''+(Get-Content -LiteralPath $path -First 1)).Trim()
        try{
            $cached=Get-Content -LiteralPath (Join-Path $data 'wallet-address.json') -Raw | ConvertFrom-Json
            $identity=(Get-FileHash -LiteralPath (Join-Path $data 'wallet.qwlt') -Algorithm SHA256).Hash
            if($value -match '^qbc1[a-z0-9]+$' -and $cached.address -eq $value -and $cached.wallet_sha256 -eq $identity){$script:address=$value}
        }catch{ }
    }
    if($previousAddress -ne $script:address){$script:balanceValue='--';$script:spendableValue='--';$script:balanceTime='Not loaded';Balance-Display}
    if ($script:address) { $receive.Text = $script:address } else { $receive.Text = 'Create or unlock your wallet to get an address' }
    $create.Text = $(if (Test-Path -LiteralPath (Join-Path $data 'wallet.qwlt')) { 'Unlock / recover address' } else { 'Create wallet' })
}
function Balance-Display {
    if ($script:hiddenBalance) { $balance.Text='******** QBTC'; $available.Text='Available: ********' }
    else { $balance.Text=$script:balanceValue + ' QBTC'; $available.Text='Available next block: ' + $script:spendableValue }
    $balanceStamp.Text = 'Balance snapshot: ' + $script:balanceTime
    $eye.AccessibleName = $(if ($script:hiddenBalance) { 'Show balance' } else { 'Hide balance' })
    $eye.Invalidate()
}
function Refresh-Balance {
    if($script:balancePoll){return}
    if(!$script:address){Tell 'Create or recover your wallet address first.';return}
    $chain=Join-Path $data 'chain.db'
    if(!(Test-Path -LiteralPath $chain)){Tell 'Start your node first to load its chain.';return}
    try{
        $info=New-Object Diagnostics.ProcessStartInfo
        $info.FileName=Join-Path $PSScriptRoot 'bin\qbtc-wallet.exe'
        $info.Arguments='balance-address '+(Quote-Arg $script:address)+' '+(Quote-Arg $chain)
        $info.UseShellExecute=$false;$info.CreateNoWindow=$true
        $info.RedirectStandardInput=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
        $process=New-Object Diagnostics.Process;$process.StartInfo=$info
        $null=$process.Start();$process.StandardInput.Close()
        $script:balancePoll=@{process=$process;address=$script:address;started=[DateTime]::UtcNow}
        $refresh.Enabled=$false;$notice.Text='Reading balance from the local chain...'
    }catch{Tell $_.Exception.Message}
}
function To-Coins([string]$atoms) { return ([decimal]$atoms / 100000000).ToString('0.00000000',[Globalization.CultureInfo]::InvariantCulture) }
function To-Atoms([string]$coins) {
    if ($coins -notmatch '^[0-9]{1,8}(\.[0-9]{1,8})?$') { throw 'Use a decimal point and up to eight decimal places.' }
    $value = [decimal]::Parse($coins,[Globalization.CultureInfo]::InvariantCulture) * 100000000
    if ($value -gt 2100000000000000) { throw 'Amount exceeds the monetary cap.' }
    return $value.ToString('0',[Globalization.CultureInfo]::InvariantCulture)
}
function Wallet-Task([hashtable]$request) {
    if ($script:task) { Tell 'Finish the current wallet operation first.'; return }
    $path = Join-Path $data ('panel-' + [guid]::NewGuid().ToString('N') + '.json')
    $request | ConvertTo-Json | Set-Content -LiteralPath $path -Encoding UTF8
    $walletArguments = '-NoProfile -ExecutionPolicy Bypass -File ' + (Quote-Arg (Join-Path $PSScriptRoot 'WALLET-ACTION.ps1')) + ' -RequestFile ' + (Quote-Arg $path)
    $process = Start-Process -FilePath $script:powershell -ArgumentList $walletArguments -PassThru
    $script:task = @{ process=$process; path=$path }
    $notice.Text = 'Complete the wallet operation in the secure password console.'
}
function Valid-IP([string]$value) {
    $parsed = $null
    return ($value -match '^([0-9]{1,3}\.){3}[0-9]{1,3}$' -and [Net.IPAddress]::TryParse($value,[ref]$parsed))
}
function Read-PanelSession([string]$path) {
    # Allow atomic replacement while this reader holds the old snapshot open.
    $stream=$null;$reader=$null
    try {
        $sharing=[IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,$sharing)
        $reader=New-Object IO.StreamReader($stream)
        return ($reader.ReadToEnd() | ConvertFrom-Json)
    } finally {
        if($reader){$reader.Dispose()}elseif($stream){$stream.Dispose()}
    }
}
function Save-TorConfig {
    $peer=$torPeerBox.Text.Trim().ToLowerInvariant()
    if($peer -and $peer -notmatch '^[a-z2-7]{56}\.onion$'){throw 'Use a Tor v3 onion address, without a URL prefix or port.'}
    @{peer=$peer} | ConvertTo-Json | Set-Content -LiteralPath $script:torConfigPath -Encoding ASCII
    return $peer
}
function Launch-TorBackend([bool]$mine,[bool]$stopOnly=$false) {
    if($mine -and !$script:address){Tell 'Create or unlock your wallet first.';return}
    try{
        $peer=$(if($stopOnly){''}else{Save-TorConfig})
        $id=[guid]::NewGuid().ToString('N')
        $path=Join-Path $data ('panel-tor-'+$id+'.json');$stopPath=$path+'.stop'
        $owner=[Diagnostics.Process]::GetCurrentProcess().StartTime.ToUniversalTime().Ticks
        $line='-NoProfile -ExecutionPolicy Bypass -File '+(Quote-Arg (Join-Path $PSScriptRoot 'PANEL-TOR.ps1'))+' -StateFile '+(Quote-Arg $path)+' -StopFile '+(Quote-Arg $stopPath)+' -OwnerPid '+$PID+' -OwnerStartTicks '+$owner
        if($peer){$line+=' -RemoteOnion '+(Quote-Arg $peer)}
        if($mine){$line+=' -Mine'}
        if($stopOnly){$line+=' -StopOnly'}
        $process=Start-Process -FilePath $script:powershell -ArgumentList $line -WindowStyle Hidden -PassThru
        $script:torTask=@{process=$process;path=$path;stop=$stopPath;mining=$mine;peer=$peer;stamp='';stopping=$false}
        $torStatus.Text=$(if($stopOnly){'Stopping the previous node session...'}else{'Starting your node through Tor...'})
        $notice.Text=$(if($stopOnly){'Stopping node and mining...'}else{'Your wallet is reused. Preparing the node...'})
    }catch{Tell $_.Exception.Message}
}
function Stop-PanelTor {
    $script:pendingTorStart=$null
    if($script:torTask -and !$script:torTask.process.HasExited){
        [IO.File]::WriteAllText($script:torTask.stop,'stop')
        $script:torTask.stopping=$true;$torStatus.Text='Stopping node, mining and Tor...'
    }else{Launch-TorBackend $false $true}
}
function Start-Node([bool]$mine) {
    if($mine -and !$script:address){Tell 'Create or unlock your wallet first.';return}
    try{
        $peer=Save-TorConfig
        if($script:torTask -and !$script:torTask.process.HasExited){
            if(!$script:torTask.stopping -and $script:torTask.mining -eq $mine -and $script:torTask.peer -eq $peer){$notice.Text='This node mode is already running.';return}
            $script:pendingTorStart=@{mining=$mine}
            [IO.File]::WriteAllText($script:torTask.stop,'stop');$script:torTask.stopping=$true
            $torStatus.Text='Changing node mode...';return
        }
        $script:pendingTorStart=$null
        Launch-TorBackend $mine
    }catch{Tell $_.Exception.Message}
}
function New-Label($parent,[string]$text,[int]$x,[int]$y,[int]$width,[int]$size=10) {
    $label = New-Object Windows.Forms.Label
    $label.Text=$text; $label.Location=New-Object Drawing.Point($x,$y); $label.Size=New-Object Drawing.Size($width,28)
    $label.Font=New-Object Drawing.Font('Segoe UI',$size); $label.ForeColor=$script:ink
    $parent.Controls.Add($label); return $label
}
function New-Button($parent,[string]$text,[int]$x,[int]$y,[int]$width) {
    $button=New-Object Windows.Forms.Button
    $button.Text=$text; $button.Location=New-Object Drawing.Point($x,$y); $button.Size=New-Object Drawing.Size($width,34)
    $button.FlatStyle='Flat'; $button.FlatAppearance.BorderColor=[Drawing.ColorTranslator]::FromHtml('#d7deea')
    $button.BackColor=[Drawing.Color]::White; $parent.Controls.Add($button); return $button
}
function New-Box($parent,[int]$x,[int]$y,[int]$width) {
    $box=New-Object Windows.Forms.TextBox; $box.Location=New-Object Drawing.Point($x,$y); $box.Size=New-Object Drawing.Size($width,26)
    $parent.Controls.Add($box); return $box
}
$form=New-Object Windows.Forms.Form
$desktopExe=Join-Path $PSScriptRoot '..\QBTC.exe'
if(Test-Path -LiteralPath $desktopExe){try{$form.Icon=[Drawing.Icon]::ExtractAssociatedIcon($desktopExe)}catch{}}
$form.Text='QBTC | Wallet + Tor + Mining'; $form.ClientSize=New-Object Drawing.Size(960,815)
$form.MinimumSize=$form.Size; $form.MaximizeBox=$false; $form.StartPosition='CenterScreen'; $form.AutoScaleMode='Dpi'
$form.BackColor=[Drawing.ColorTranslator]::FromHtml('#f5f6f8'); $form.Font=New-Object Drawing.Font('Segoe UI',10)
$null=New-Label $form 'QBTC' 24 16 170 23
$state=New-Label $form 'Checking node...' 720 28 215 10
$null=New-Label $form 'Experimental network. Official QBTC launch is pending.' 26 58 720 10
$balance=New-Label $form '******** QBTC' 26 96 700 26; $balance.Height=48
$available=New-Label $form 'Available: ********' 28 150 750 10
$balanceStamp=New-Label $form 'Balance snapshot: Not loaded' 28 177 750 9
$eye=New-Button $form '' 824 103 52
$eye.add_Paint({ param($sender,$event)
    $pen=New-Object Drawing.Pen($script:ink,1.8)
    $event.Graphics.SmoothingMode='AntiAlias'; $event.Graphics.DrawEllipse($pen,14,10,24,13); $event.Graphics.DrawEllipse($pen,24,14,5,5)
    if ($script:hiddenBalance) { $event.Graphics.DrawLine($pen,12,26,40,7) }; $pen.Dispose()
})
$eye.add_Click({ $script:hiddenBalance=!$script:hiddenBalance; Balance-Display; if(!$script:hiddenBalance -and $script:balanceTime -eq 'Not loaded'){Refresh-Balance} })
$refresh=New-Button $form 'Refresh balance' 732 153 170
$refresh.add_Click({ Refresh-Balance })
$receive=New-Box $form 28 217 690; $receive.ReadOnly=$true; $receive.AccessibleName='Receiving address'
$copy=New-Button $form 'Copy address' 732 212 170
$copy.add_Click({ if ($script:address) { [Windows.Forms.Clipboard]::SetText($script:address); $notice.Text='Receiving address copied.' } })
$create=New-Button $form 'Create wallet' 28 260 215
$create.add_Click({ Wallet-Task @{action=$(if (Test-Path -LiteralPath (Join-Path $data 'wallet.qwlt')) {'address'} else {'create'})} })
$backup=New-Button $form 'Encrypted backup' 257 260 190
$backup.add_Click({ $dialog=New-Object Windows.Forms.SaveFileDialog; $dialog.Filter='Encrypted wallet (*.qwlt)|*.qwlt'; $dialog.FileName='QBTC-wallet-backup.qwlt'; $dialog.OverwritePrompt=$false; $dialog.Title='Choose a NEW backup filename (existing backups are preserved)'; if ($dialog.ShowDialog() -eq 'OK') { if(Test-Path -LiteralPath $dialog.FileName){Tell 'That backup already exists. Choose a new filename.'}else{Wallet-Task @{action='backup';destination=$dialog.FileName}} }; $dialog.Dispose() })
$send=New-Button $form 'Send QBTC' 461 260 165
$send.add_Click({
    if (!$script:online) { Tell 'Start your node before sending.'; return }
    $dialog=New-Object Windows.Forms.Form; $dialog.Text='Send QBTC'; $dialog.ClientSize=New-Object Drawing.Size(620,255); $dialog.StartPosition='CenterParent'; $dialog.Font=$form.Font
    $null=New-Label $dialog 'Recipient (qbc1...)' 20 15 570; $recipient=New-Box $dialog 20 45 575
    $null=New-Label $dialog 'Amount QBTC' 20 84 270; $amount=New-Box $dialog 20 114 270
    $null=New-Label $dialog 'Fee QBTC' 315 84 270; $fee=New-Box $dialog 315 114 280; $fee.Text='0.00001000'
    $null=New-Label $dialog 'Your password is entered privately in the wallet console.' 20 155 575 9
    $confirm=New-Button $dialog 'Review and sign' 405 197 190
    $confirm.add_Click({ try {
        if ($recipient.Text.Trim() -notmatch '^qbc1[a-z0-9]+$') { throw 'Enter a QBTC mainnet address.' }
        $atoms=To-Atoms $amount.Text.Trim(); $feeAtoms=To-Atoms $fee.Text.Trim()
        if ([decimal]$atoms -le 0) { throw 'Amount must be positive.' }
        if([decimal]$feeAtoms -gt [decimal]$atoms -or [decimal]$feeAtoms -gt 100000000){
            if([Windows.Forms.MessageBox]::Show('WARNING: this fee exceeds the payment or is greater than 1 QBTC. Review it carefully. Continue?','High transaction fee','YesNo','Warning') -ne 'Yes'){return}
        }
        $message='Send ' + $amount.Text.Trim() + ' QBTC to ' + $recipient.Text.Trim() + "?`nFee: " + $fee.Text.Trim() + ' QBTC'
        if ([Windows.Forms.MessageBox]::Show($message,'Review payment','OKCancel') -ne 'OK') { return }
        Wallet-Task @{action='send';address=$recipient.Text.Trim();amount=$atoms;fee=$feeAtoms;endpoint='127.0.0.1:19021'}
        $dialog.Close()
    } catch { Tell $_.Exception.Message } })
    $null=$dialog.ShowDialog($form); $dialog.Dispose()
})
$verifyBackup=New-Button $form 'Verify backup' 0 0 170
$restoreBackup=New-Button $form 'Restore backup' 0 0 170
$verifyBackup.add_Click({
    $file=New-Object Windows.Forms.OpenFileDialog;$file.Filter='Encrypted wallet (*.qwlt)|*.qwlt'
    if($file.ShowDialog() -eq 'OK'){Wallet-Task @{action='verify-backup';source=$file.FileName}};$file.Dispose()
})
$restoreBackup.add_Click({
    if(Test-Path -LiteralPath (Join-Path $data 'wallet.qwlt')){Tell 'Your active wallet is preserved. Use Verify backup to check a backup; Restore is available when no active wallet exists.';return}
    $file=New-Object Windows.Forms.OpenFileDialog;$file.Filter='Encrypted wallet (*.qwlt)|*.qwlt'
    if($file.ShowDialog() -eq 'OK'){Wallet-Task @{action='restore';source=$file.FileName}};$file.Dispose()
})
$retry=New-Button $form 'Retry signed payment' 640 260 260
$retry.add_Click({
    if(!$script:online){Tell 'Start your node first.';return}
    if(!(Test-Path -LiteralPath (Join-Path $data 'pending-payment.json'))){Tell 'No unresolved signed payment.';return}
    if([Windows.Forms.MessageBox]::Show('Resubmit the exact saved transaction? This does not create or sign another payment.','Retry signed payment','OKCancel') -eq 'OK'){Wallet-Task @{action='retry';endpoint='127.0.0.1:19021'}}
})
$cards=@{}
$names=@('HASHRATE (SESSION AVG)','HASH ATTEMPTS (SESSION)','CHAIN HEIGHT','MEMPOOL')
for ($i=0; $i -lt 4; $i++) {
    $panel=New-Object Windows.Forms.Panel; $panel.BackColor=[Drawing.Color]::White; $panel.BorderStyle='FixedSingle'
    $panel.Location=New-Object Drawing.Point((28+$i*225),318); $panel.Size=New-Object Drawing.Size(211,85); $form.Controls.Add($panel)
    $null=New-Label $panel $names[$i] 12 9 195 8
    $cards[$i]=New-Label $panel '--' 12 34 190 20; $cards[$i].Height=40; $cards[$i].ForeColor=$script:accent
}
$chart=New-Object Windows.Forms.DataVisualization.Charting.Chart
$chart.Location=New-Object Drawing.Point(28,418); $chart.Size=New-Object Drawing.Size(886,140); $chart.BackColor=[Drawing.Color]::White
$area=New-Object Windows.Forms.DataVisualization.Charting.ChartArea
$area.AxisX.LabelStyle.Enabled=$false; $area.AxisX.MajorGrid.Enabled=$false; $area.AxisY.Minimum=0; $area.AxisY.MajorGrid.LineColor=[Drawing.Color]::Gainsboro
$chart.ChartAreas.Add($area)
$series=New-Object Windows.Forms.DataVisualization.Charting.Series
$series.ChartType='Line'; $series.BorderWidth=2; $series.Color=$script:accent; $chart.Series.Add($series)
$null=$chart.Titles.Add('Session average hashrate / H/s - recent connected samples'); $form.Controls.Add($chart)
$null=New-Label $form 'THIS PC IPv4' 28 572 260 8; $bind=New-Box $form 28 598 270
$null=New-Label $form 'PEER IPv4 (OPTIONAL)' 325 572 260 8; $seed=New-Box $form 325 598 270
$null=New-Label $form 'PUBLIC IPv4 (INTERNET ONLY)' 622 572 292 8; $public=New-Box $form 622 598 292
$configPath=Join-Path $data 'network.json'
if (Test-Path -LiteralPath $configPath) { try { $config=Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json; $bind.Text=$config.bind; $seed.Text=$config.seed; $public.Text=$config.public } catch { Tell 'Network config could not be read. Enter your IPv4 settings.' } }
$save=New-Button $form 'Save connections' 28 642 180
$save.add_Click({try{$null=Save-TorConfig;$notice.Text='Tor peer saved. Start the node or mining to use it.'}catch{Tell $_.Exception.Message}})
$start=New-Button $form 'Start node' 222 642 160; $start.add_Click({ Start-Node $false })
$mine=New-Button $form 'Start node + mine' 396 642 205; $mine.ForeColor=$script:accent; $mine.add_Click({ Start-Node $true })
$stop=New-Button $form 'Stop node' 615 642 145
$stop.add_Click({ Stop-PanelTor })
$notice=New-Label $form 'Test network - your wallet and your node. Tor connects without router changes.' 28 696 890 9; $notice.Height=48
$null=New-Label $form 'P2P 19021 | Local control 19121 | Rewards mature after 100 blocks. No profit estimate.' 28 747 890 9
$null=New-Label $form 'Experimental panel. Password operations open the wallet console; passwords are never stored by this panel.' 28 776 900 8
. (Join-Path $PSScriptRoot 'PANEL-LAYOUT.ps1')
Read-Address; Balance-Display
$timer=New-Object Windows.Forms.Timer; $timer.Interval=1000
$timer.add_Tick({
    $timer.Stop()
    try {
        if ($script:torTask) {
            $job=$script:torTask
            if (Test-Path -LiteralPath $job.path) {
                try{
                    $session=Read-PanelSession $job.path
                    if($session.bind){$bind.Text=$session.bind}
                    if($session.onion){$ownOnionBox.Text=$session.onion}
                    if($session.time -ne $job.stamp){$torStatus.Text=$session.message;$job.stamp=$session.time}
                }catch{ }
            }
            if($job.process.HasExited){
                if(!(Test-Path -LiteralPath $job.path)){$torStatus.Text='Could not start the Tor service. No result was returned.'}
                foreach($path in @($job.path,$job.stop,($job.path+'.bak'),($job.path+'.tmp'))){Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue}
                $job.process.Dispose();$script:torTask=$null
                if($script:pendingTorStart){$request=$script:pendingTorStart;$script:pendingTorStart=$null;Launch-TorBackend ([bool]$request.mining)}
            }
        }
        if($script:balancePoll){
            $job=$script:balancePoll;$process=$job.process
            if($process.HasExited){
                try{
                    $text=$process.StandardOutput.ReadToEnd();$errorText=$process.StandardError.ReadToEnd()
                    if($job.address -eq $script:address){
                        if($process.ExitCode -eq 0 -and $text -match '^total_atoms=(\d+) spendable_next_block_atoms=(\d+)\s*$'){
                            $script:balanceValue=To-Coins $Matches[1];$script:spendableValue=To-Coins $Matches[2]
                            $script:balanceTime=[DateTime]::Now.ToString('HH:mm:ss');Balance-Display
                            $notice.Text='Balance updated.'
                        }else{$notice.Text='Balance could not be read. Try Refresh again after the next block.'}
                    }
                }finally{$process.Dispose();$script:balancePoll=$null;$refresh.Enabled=$true}
            }elseif(([DateTime]::UtcNow-$job.started).TotalSeconds -gt 30){
                try{$process.Kill()}catch{};$process.Dispose();$script:balancePoll=$null;$refresh.Enabled=$true
                $notice.Text='Balance lookup timed out. Try Refresh again.'
            }
        }
        if ($script:task -and $script:task.process.HasExited) {
            $resultFile=$script:task.path+'.result'
            if (Test-Path -LiteralPath $resultFile) {
                $result=Get-Content -LiteralPath $resultFile -Raw | ConvertFrom-Json
                if ($result.ok) {
                    Read-Address; $notice.Text='Wallet operation complete: '+$result.action
                    if ($result.action -in @('send','retry','verify-backup','restore')) { Tell $result.text }
                } else { Tell $result.text; $notice.Text='Wallet operation failed.' }
                Remove-Item -LiteralPath $resultFile -Force
            } else { $notice.Text='Wallet console closed without a result.' }
            Remove-Item -LiteralPath $script:task.path -Force -ErrorAction SilentlyContinue
            $script:task.process.Dispose(); $script:task=$null
        }
        if ($script:poll) {
            if (!$script:poll.HasExited) { return }
            $output=$script:poll.StandardOutput.ReadToEnd(); $errorText=$script:poll.StandardError.ReadToEnd()
            $success=$script:poll.ExitCode -eq 0; $script:poll.Dispose(); $script:poll=$null
            $values=@{}; foreach ($match in [regex]::Matches($output,'([a-z_]+)=([^\s]+)')) { $values[$match.Groups[1].Value]=$match.Groups[2].Value }
            if ($success -and $values['network'] -eq 'mainnet') {
                $script:online=$true; $state.Text=$(if ($values['auto_mining'] -eq 'on') {'MINING | TEST NETWORK'} else {'NODE ONLINE | TEST NETWORK'}); $state.ForeColor=[Drawing.Color]::SeaGreen
                $cards[0].Text=$values['last_pipeline_hps']+' H/s'; $cards[1].Text=$values['last_hash_attempts']; $cards[2].Text=$values['height']; $cards[3].Text=$values['mempool']
                if ($script:lastHeight -ne $values['height'] -and $script:balanceTime -ne 'Not loaded') { $balanceStamp.Text='Balance snapshot: '+$script:balanceTime+' | Refresh for current chain' }
                $script:lastHeight=$values['height']; $script:samples++
                $null=$series.Points.AddXY($script:samples,[double]$values['last_pipeline_hps']); if ($series.Points.Count -gt 120) {$series.Points.RemoveAt(0)}
            } else {
                $script:online=$false; $state.Text='NODE OFFLINE'; $state.ForeColor=[Drawing.Color]::DimGray
                foreach ($card in $cards.Values) { $card.Text='--' }
                $series.Points.Clear()
            }
        }
        if([DateTime]::UtcNow -lt $script:nextStatusPoll){return}
        $script:nextStatusPoll=[DateTime]::UtcNow.AddSeconds($(if($script:online){3}else{10}))
        $info=New-Object Diagnostics.ProcessStartInfo; $info.FileName=$script:peer; $info.Arguments='local-status 19121 --control-file '+(Quote-Arg (Join-Path $data 'chain.db.control'))
        $info.UseShellExecute=$false; $info.CreateNoWindow=$true; $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
        $script:poll=New-Object Diagnostics.Process; $script:poll.StartInfo=$info; $null=$script:poll.Start()
    } catch { $notice.Text=$_.Exception.Message }
    finally { if (!$form.IsDisposed) { $timer.Start() } }
})
$form.add_FormClosing({
    $timer.Stop()
    $script:pendingTorStart=$null
    if($script:torTask -and !$script:torTask.process.HasExited){try{[IO.File]::WriteAllText($script:torTask.stop,'stop')}catch{}}
    if($script:poll){try{if(!$script:poll.HasExited){$script:poll.Kill()}}catch{}}
    if($script:balancePoll){try{if(!$script:balancePoll.process.HasExited){$script:balancePoll.process.Kill()};$script:balancePoll.process.Dispose()}catch{}}
})
$timer.Start()
try { $null=$form.ShowDialog() } finally { $timer.Dispose(); $form.Dispose() }
