# Responsive wallet, network and mining controls.
# Every section owns its rows; wide labels can never paint over action buttons.
function Layout-Table([int]$columns) {
    $table=New-Object Windows.Forms.TableLayoutPanel
    $table.ColumnCount=$columns; $table.RowCount=0; $table.AutoSize=$true
    $table.AutoSizeMode='GrowAndShrink'; $table.Dock='Top'
    $table.Margin=New-Object Windows.Forms.Padding(0,0,0,14)
    return $table
}
function Layout-Row($table,$control,[int]$row,[int]$column=0,[int]$span=1) {
    if ($table.RowCount -le $row) {
        while ($table.RowCount -le $row) {
            $table.RowCount++
            $null=$table.RowStyles.Add((New-Object Windows.Forms.RowStyle([Windows.Forms.SizeType]::AutoSize)))
        }
    }
    $control.Dock='Fill'; $control.Margin=New-Object Windows.Forms.Padding(0,3,10,5)
    $control.MinimumSize=New-Object Drawing.Size(0,0)
    $table.Controls.Add($control,$column,$row)
    if ($span -gt 1) { $table.SetColumnSpan($control,$span) }
}
function Layout-Text($control,[int]$extra=14) {
    $control.AutoSize=$false
    $control.Height=[int][Math]::Ceiling($control.Font.GetHeight())+$extra
    $control.TextAlign='MiddleLeft'
}
$form.SuspendLayout()
$originalControls=@($form.Controls)
$form.Controls.Clear()
$form.AutoScaleDimensions=New-Object Drawing.SizeF(96,96)
$form.AutoScaleMode='Dpi'
$form.MaximizeBox=$true
$form.MinimumSize=New-Object Drawing.Size(760,580)
$workArea=[Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$form.ClientSize=New-Object Drawing.Size(([Math]::Min(1000,$workArea.Width-70)),([Math]::Min(900,$workArea.Height-100)))
$form.Text='QBTC | Wallet + Tor + Mining'
$script:viewport=New-Object Windows.Forms.Panel
$viewport.Dock='Fill'; $viewport.AutoScroll=$true
$form.Controls.Add($viewport)
$script:body=Layout-Table 1
$body.Padding=New-Object Windows.Forms.Padding(22,16,12,16)
$body.Margin=New-Object Windows.Forms.Padding(0)
$body.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Percent,100)))
$viewport.Controls.Add($body)

$header=Layout-Table 2
$header.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Percent,65)))
$header.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Percent,35)))
$title=New-Label $form 'QBTC' 0 0 200 23
Layout-Text $title; Layout-Text $state
$state.TextAlign='MiddleRight'
Layout-Row $header $title 0 0
Layout-Row $header $state 0 1
$subtitle=New-Label $form 'Experimental network. Official QBTC launch is pending.' 0 0 600 10
Layout-Text $subtitle
Layout-Row $header $subtitle 1 0 2
Layout-Row $body $header 0

$walletLayout=Layout-Table 2
$walletLayout.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Percent,100)))
$walletLayout.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Absolute,190)))
Layout-Text $balance 22; Layout-Text $available; Layout-Text $balanceStamp
$eye.Dock='None'; $eye.Anchor='Right'; $eye.Size=New-Object Drawing.Size(52,38)
Layout-Row $walletLayout $balance 0 0
Layout-Row $walletLayout $eye 0 1
$eye.Dock='None'; $eye.Anchor='Right'
Layout-Row $walletLayout $available 1 0
Layout-Row $walletLayout $refresh 1 1
Layout-Row $walletLayout $balanceStamp 2 0 2
Layout-Row $walletLayout $receive 3 0
$receive.Anchor='Left,Right'; $receive.Dock='None'
Layout-Row $walletLayout $copy 3 1
$walletActions=New-Object Windows.Forms.FlowLayoutPanel
$walletActions.AutoSize=$true; $walletActions.WrapContents=$true
foreach ($button in @($create,$backup,$verifyBackup,$restoreBackup,$send,$retry)) {
    $button.AutoSize=$true; $button.MinimumSize=New-Object Drawing.Size(150,38)
    $button.Margin=New-Object Windows.Forms.Padding(0,4,12,4)
    $walletActions.Controls.Add($button)
}
Layout-Row $walletLayout $walletActions 4 0 2
Layout-Row $body $walletLayout 1

$metrics=Layout-Table 4
$metricNames=@('HASHRATE (AVG)','HASH ATTEMPTS','CHAIN HEIGHT','MEMPOOL')
for ($i=0; $i -lt 4; $i++) {
    $metrics.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Percent,25)))
    $metricPanel=$cards[$i].Parent
    $heading=@($metricPanel.Controls | Where-Object { $_ -ne $cards[$i] })[0]
    $heading.Text=$metricNames[$i]
    $metricPanel.Controls.Clear()
    $metricPanel.Padding=New-Object Windows.Forms.Padding(10,6,10,8)
    $metricPanel.AutoSize=$true
    $inside=Layout-Table 1; $inside.Margin=New-Object Windows.Forms.Padding(0)
    $inside.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Percent,100)))
    Layout-Text $heading; Layout-Text $cards[$i] 16
    Layout-Row $inside $heading 0
    Layout-Row $inside $cards[$i] 1
    $heading.Margin=New-Object Windows.Forms.Padding(0)
    $cards[$i].Margin=New-Object Windows.Forms.Padding(0)
    $metricPanel.Controls.Add($inside)
    Layout-Row $metrics $metricPanel 0 $i
}
Layout-Row $body $metrics 2
$chart.Height=165
Layout-Row $body $chart 3

$network=Layout-Table 2
$network.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Percent,100)))
$network.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Absolute,170)))
$caption=New-Label $form 'OPTIONAL EXTRA PEER (AUTOMATIC DISCOVERY IS ON)' 0 0 600 8;Layout-Text $caption
Layout-Row $network $caption 0 0 2
$torPeerBox=New-Box $form 0 0 650
if(Test-Path -LiteralPath $script:torConfigPath){try{$config=Get-Content -LiteralPath $script:torConfigPath -Raw | ConvertFrom-Json;$torPeerBox.Text=$config.peer}catch{}}
if($TorPeer){$torPeerBox.Text=$TorPeer}
Layout-Row $network $torPeerBox 1 0
$save.Text='Save extra peer';Layout-Row $network $save 1 1
$caption=New-Label $form 'YOUR NODE ADDRESS' 0 0 500 8;Layout-Text $caption
Layout-Row $network $caption 2 0 2
$ownOnionBox=New-Box $form 0 0 650;$ownOnionBox.ReadOnly=$true
$onionPath=Join-Path $data 'onion-address.txt'
if(Test-Path -LiteralPath $onionPath){$ownOnionBox.Text=(''+(Get-Content -LiteralPath $onionPath -First 1)).Trim()}
Layout-Row $network $ownOnionBox 3 0
$copyOnion=New-Button $form 'Copy node address' 0 0 150
$copyOnion.add_Click({if($ownOnionBox.Text -match '^[a-z2-7]{56}\.onion$'){[Windows.Forms.Clipboard]::SetText($ownOnionBox.Text)}})
Layout-Row $network $copyOnion 3 1
$torStatus=New-Label $form 'Tor is managed here. Start node or Start mining. Closing this panel stops its node.' 0 0 700 9;$torStatus.Height=64
Layout-Row $network $torStatus 4 0 2
Layout-Row $body $network 4
$miningControls=Layout-Table 2
$miningControls.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Absolute,200)))
$miningControls.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle([Windows.Forms.SizeType]::Percent,100)))
$modeLabel=New-Label $form 'MINING POWER' 0 0 190 9;Layout-Text $modeLabel
Layout-Row $miningControls $modeLabel 0 0
$miningModeBox=New-Object Windows.Forms.ComboBox
$miningModeBox.DropDownStyle='DropDownList'
$null=$miningModeBox.Items.AddRange([object[]]@('Low','Medium','High'))
$modePath=Join-Path $data 'mining-mode.txt'
$initialMode='medium'
if(Test-Path -LiteralPath $modePath){$savedMode=(''+(Get-Content -LiteralPath $modePath -Raw)).Trim().ToLowerInvariant();if($savedMode -in @('low','medium','high')){$initialMode=$savedMode}}
$miningModeBox.SelectedItem=(Get-Culture).TextInfo.ToTitleCase($initialMode)
[IO.File]::WriteAllText($modePath,$initialMode,[Text.Encoding]::ASCII)
$miningModeBox.add_SelectedIndexChanged({
    try{[IO.File]::WriteAllText((Join-Path $data 'mining-mode.txt'),$miningModeBox.SelectedItem.ToString().ToLowerInvariant(),[Text.Encoding]::ASCII);$notice.Text='Mining power saved. Applied within one second while mining; the node stays online.'}catch{Tell $_.Exception.Message}
})
Layout-Row $miningControls $miningModeBox 0 1
$modeHint=New-Label $form 'Low: 1 CPU thread | Medium: half | High: all except one. Continuous mining in every mode.' 0 0 700 9
$modeHint.Height=44;Layout-Row $miningControls $modeHint 1 0 2
Layout-Row $body $miningControls 5
$nodeActions=New-Object Windows.Forms.FlowLayoutPanel
$nodeActions.AutoSize=$true;$nodeActions.WrapContents=$true
$start.Text='Start node';$mine.Text='Start mining';$stop.Text='Stop node / mining'
foreach($button in @($start,$mine,$stop)){
    $button.AutoSize=$true;$button.MinimumSize=New-Object Drawing.Size(150,40)
    $button.Margin=New-Object Windows.Forms.Padding(0,4,12,4);$nodeActions.Controls.Add($button)
}
Layout-Row $body $nodeActions 6
# Native node and all local wallet submissions use loopback, independent of adapters.
$bind.Text='127.0.0.1'
$notice.Height=58; $notice.TextAlign='MiddleLeft'
Layout-Row $body $notice 7
$footer=New-Label $form 'P2P 19021 | Local control 19121 | Rewards mature after 100 blocks. No profit estimate.' 0 0 700 9
$footer.Height=42; $footer.TextAlign='MiddleLeft'
Layout-Row $body $footer 8
$securityNote=New-Label $form 'Experimental panel. Password operations open the wallet console; passwords are never stored by this panel.' 0 0 700 8
$securityNote.Height=42; $securityNote.TextAlign='MiddleLeft'
Layout-Row $body $securityNote 9

# The scroll viewport owns the available width. Height is content-driven.
$body.Width=$viewport.ClientSize.Width-[Windows.Forms.SystemInformation]::VerticalScrollBarWidth
$viewport.add_Resize({ $body.Width=[Math]::Max(650,$viewport.ClientSize.Width-[Windows.Forms.SystemInformation]::VerticalScrollBarWidth) })
foreach ($old in $originalControls) { if (!$old.Parent -and $old -ne $bind -and $old -ne $seed -and $old -ne $public) { $old.Dispose() } }
$form.ResumeLayout($true)
