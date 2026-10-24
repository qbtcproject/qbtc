$ErrorActionPreference='Stop'
try {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [Windows.Forms.Application]::EnableVisualStyles()
    $exe=Join-Path $PSScriptRoot 'bin\qbtc-peer.exe'
    $folder=Join-Path $env:LOCALAPPDATA 'QBTC-calibration-v1'
    $null=New-Item -ItemType Directory -Path $folder -Force
    $script:sampleProcess=$null;$script:sampleText='';$script:started=$null
    $form=New-Object Windows.Forms.Form
    $form.Text='QBTC | CPU calibration';$form.ClientSize=New-Object Drawing.Size(620,380)
    $form.StartPosition='CenterScreen';$form.FormBorderStyle='FixedDialog';$form.MaximizeBox=$false
    $form.Font=New-Object Drawing.Font('Segoe UI',10)
    $form.BackColor=[Drawing.ColorTranslator]::FromHtml('#f6f5f0')
    $icon=Join-Path $PSScriptRoot '..\QBTC-Calibrate.exe'
    if(Test-Path $icon){try{$form.Icon=[Drawing.Icon]::ExtractAssociatedIcon($icon)}catch{}}
    $intro=New-Object Windows.Forms.Label
    $intro.Text='Optional CPU throughput check for QBTC development. This uses the same miner as the experimental desktop. No coins are mined; no wallet or network connection is used.'
    $intro.SetBounds(24,20,570,75);$form.Controls.Add($intro)
    $start=New-Object Windows.Forms.Button;$start.Text='Measure CPU (30 seconds)'
    $start.SetBounds(24,108,290,42);$form.Controls.Add($start)
    $hint=New-Object Windows.Forms.Label;$hint.Text='Stop QBTC mining and other heavy apps first. Uses High mode: all CPU threads except one.'
    $hint.SetBounds(24,162,570,48);$form.Controls.Add($hint)
    $status=New-Object Windows.Forms.Label;$status.Text='Ready. Run once on each PC.'
    $status.SetBounds(24,217,570,70);$form.Controls.Add($status)
    $copy=New-Object Windows.Forms.Button;$copy.Text='Copy result';$copy.Enabled=$false
    $copy.SetBounds(24,308,180,38);$form.Controls.Add($copy)
    $show=New-Object Windows.Forms.Button;$show.Text='Open results folder'
    $show.SetBounds(220,308,220,38);$form.Controls.Add($show)
    $show.add_Click({$null=Start-Process explorer.exe -ArgumentList ('"'+$folder+'"')})
    $copy.add_Click({if($script:sampleText){[Windows.Forms.Clipboard]::SetText($script:sampleText);$status.Text='Result copied. Paste this result into the QBTC conversation.'}})
    $start.add_Click({
        if($script:sampleProcess){return}
        try {
            $active=@(Get-CimInstance Win32_Process -Filter "Name='qbtc-peer.exe'" -ErrorAction SilentlyContinue | Where-Object {$_.CommandLine -match '\bserve\b' -and $_.CommandLine -match '--mine\s'})
            if($active.Count){throw 'Stop mining or close the QBTC panel before measuring this PC.'}
            $info=New-Object Diagnostics.ProcessStartInfo
            $info.FileName=$exe;$info.Arguments='benchmark --seconds 30 --mining-mode high'
            $info.UseShellExecute=$false;$info.CreateNoWindow=$true
            $info.RedirectStandardInput=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
            $process=New-Object Diagnostics.Process;$process.StartInfo=$info
            $null=$process.Start();$process.StandardInput.Close()
            $script:sampleProcess=$process;$script:started=[DateTime]::UtcNow
            $start.Enabled=$false;$copy.Enabled=$false;$status.Text='Measuring CPU...'
        } catch {$status.Text=$_.Exception.Message}
    })
    $timer=New-Object Windows.Forms.Timer;$timer.Interval=250
    $timer.add_Tick({
        if(!$script:sampleProcess){return}
        try {
            $process=$script:sampleProcess
            if(!$process.HasExited){
                $elapsed=([DateTime]::UtcNow-$script:started).TotalSeconds
                if($elapsed -gt 90){$process.Kill();throw 'Measurement timed out. Try again.'}
                $status.Text='Measuring CPU... '+[int][Math]::Min(30,$elapsed)+' / 30 seconds';return
            }
            $text=$process.StandardOutput.ReadToEnd();$errorText=$process.StandardError.ReadToEnd()
            if($process.ExitCode -ne 0){throw ('Measurement failed: '+$errorText)}
            $sample=$text | ConvertFrom-Json
            if($sample.schema -ne 'qbtc-cpu-calibration-v1' -or !$sample.benchmark_only -or $sample.network -ne 'none' -or $sample.hashes -le 0 -or $sample.elapsed_us -lt 30000000){throw 'Invalid measurement result.'}
            $sample | Add-Member NoteProperty peer_binary_sha256 ((Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant())
            $sample | Add-Member NoteProperty run_id ([guid]::NewGuid().ToString('N'))
            $sample | Add-Member NoteProperty generated_utc ([DateTime]::UtcNow.ToString('o'))
            $script:sampleText=$sample | ConvertTo-Json -Compress
            $path=Join-Path $folder ('calibration-'+$sample.run_id+'.json')
            [IO.File]::WriteAllText($path,$script:sampleText,(New-Object Text.UTF8Encoding($false)))
            $hps=[Math]::Round([double]$sample.hashes*1000000/[double]$sample.elapsed_us)
            $status.Text=$hps.ToString('N0')+' H/s | '+$sample.threads+' CPU threads.'+"`r`n"+'Saved. Click Copy result.'
            $copy.Enabled=$true
        } catch {$status.Text=$_.Exception.Message}
        finally {
            if($script:sampleProcess -and $script:sampleProcess.HasExited){$script:sampleProcess.Dispose();$script:sampleProcess=$null;$start.Enabled=$true}
        }
    })
    $form.add_FormClosing({$timer.Stop();if($script:sampleProcess){try{if(!$script:sampleProcess.HasExited){$script:sampleProcess.Kill()};$script:sampleProcess.Dispose()}catch{}}})
    $timer.Start()
    try{$null=$form.ShowDialog()}finally{$timer.Dispose();$form.Dispose()}
} catch {
    Add-Type -AssemblyName System.Windows.Forms
    $null=[Windows.Forms.MessageBox]::Show($_.Exception.Message,'QBTC calibration error')
    exit 32
}
