param([Parameter(Mandatory=$true)][string]$RequestFile)
$ErrorActionPreference = 'Stop'
$data = Join-Path $env:LOCALAPPDATA 'QBTC-test-v2'
$wallet = Join-Path $data 'wallet.qwlt'
$chain = Join-Path $data 'chain.db'
$exe = Join-Path $PSScriptRoot 'bin\qbtc-wallet.exe'
$response = $RequestFile + '.result'
$pendingFile=Join-Path $data 'pending-payment.json'
$walletActionLock=$null
function Cache-WalletAddress([string]$Address) {
    $identity=(Get-FileHash -LiteralPath $wallet -Algorithm SHA256).Hash
    $Address | Set-Content -LiteralPath (Join-Path $data 'address.txt') -Encoding ASCII
    @{address=$Address;wallet_sha256=$identity} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $data 'wallet-address.json') -Encoding ASCII
}
function Save-PendingPayment([string]$Path) {
    $record=@{file=[IO.Path]::GetFileName($Path);sha256=$null}
    if(Test-Path -LiteralPath $Path){$record.sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
    $temp=$pendingFile+'.tmp'
    [IO.File]::WriteAllText($temp,($record | ConvertTo-Json -Compress))
    if(Test-Path -LiteralPath $pendingFile){[IO.File]::Replace($temp,$pendingFile,($pendingFile+'.bak'))}else{[IO.File]::Move($temp,$pendingFile)}
}
function Resolve-ConfirmedPayment {
    if(!(Test-Path -LiteralPath $pendingFile)){return $false}
    $record=Get-Content -LiteralPath $pendingFile -Raw | ConvertFrom-Json
    if($record.file -notmatch '^payment-[a-f0-9]{32}\.tx$'){throw 'Invalid pending payment record.'}
    $tx=Join-Path $data $record.file
    if(!(Test-Path -LiteralPath $tx)){throw 'Pending signed payment file is missing. Review it before making another payment.'}
    if($record.sha256 -and (Get-FileHash -LiteralPath $tx -Algorithm SHA256).Hash -ne $record.sha256){throw 'Pending signed payment file changed.'}
    $status=@(& $exe payment-status $chain $tx)
    if($LASTEXITCODE -ne 0){throw 'Could not verify the previous payment in the local chain. No new payment was signed.'}
    if(($status -join ' ') -match '^confirmed height=([0-9]+) txid=([a-f0-9]{128})'){
        # Only confirmed, identity-checked files are cleaned; unresolved signatures are never deleted.
        Remove-Item -LiteralPath $tx
        Remove-Item -LiteralPath $pendingFile
        Remove-Item -LiteralPath ($pendingFile+'.bak') -ErrorAction SilentlyContinue
        return $true
    }
    return $false
}
function Submit-PendingPayment {
    if(Resolve-ConfirmedPayment){return @('The saved payment is already confirmed in your local chain. It was not submitted again.')}
    $record=Get-Content -LiteralPath $pendingFile -Raw | ConvertFrom-Json
    if($record.file -notmatch '^payment-[a-f0-9]{32}\.tx$'){throw 'Invalid saved payment record. No transaction was submitted.'}
    $tx=Join-Path $data $record.file
    if(!(Test-Path -LiteralPath $tx -PathType Leaf)){throw 'The saved payment file is missing. Review its status before creating another payment.'}
    $actual=(Get-FileHash -LiteralPath $tx -Algorithm SHA256).Hash
    if($record.sha256 -and $actual -ne $record.sha256){throw 'The saved payment changed. It was not submitted.'}
    # A crash immediately after signing may leave the initial record without its hash.
    Save-PendingPayment $tx
    $submitted=@(& (Join-Path $PSScriptRoot 'bin\qbtc-peer.exe') submit '127.0.0.1:19021' $tx)
    if($LASTEXITCODE -ne 0){throw "Signed payment saved at $tx. Submission is unresolved. Use Retry signed payment to resend the SAME transaction; do not make a replacement. Node response: $($submitted -join ' ')"}
    return @($submitted)+@('Accepted by your node. Confirmation in a block is still pending.')
}
try {
    $request = Get-Content -LiteralPath $RequestFile -Raw | ConvertFrom-Json
    New-Item -ItemType Directory -Path $data -Force | Out-Null
    $walletActionLock=[IO.File]::Open((Join-Path $data 'wallet-action.lock'),'OpenOrCreate','ReadWrite','None')
    $text = @()
    switch ($request.action) {
        'create' {
            if (Test-Path -LiteralPath $wallet) { throw 'A wallet already exists. It will not be overwritten.' }
            $text = @(& $exe create $wallet)
            if ($LASTEXITCODE -ne 0) { throw 'Wallet creation failed.' }
            $address = ([string]$text[0]).Trim()
            if ($address -notmatch '^qbc1[a-z0-9]+$') { throw 'Invalid mainnet address.' }
            Cache-WalletAddress $address
        }
        'address' {
            $text = @(& $exe address $wallet)
            if ($LASTEXITCODE -ne 0) { throw 'Could not unlock wallet.' }
            $address = ([string]$text[0]).Trim()
            if ($address -notmatch '^qbc1[a-z0-9]+$') { throw 'Invalid mainnet address.' }
            Cache-WalletAddress $address
        }
        'verify-backup' {
            $text=@(& $exe address ([string]$request.source))
            if($LASTEXITCODE -ne 0){throw 'Backup could not be decrypted.'}
            $backupAddress=([string]$text[0]).Trim()
            if($backupAddress -notmatch '^qbc1[a-z0-9]+$'){throw 'Backup returned an invalid receiving address.'}
            if(Test-Path -LiteralPath $wallet){
                $bindingPath=Join-Path $data 'wallet-address.json'
                if(!(Test-Path -LiteralPath $bindingPath)){throw 'Backup opens, but its ownership was not compared. Unlock / recover address for the active wallet, then verify again.'}
                $binding=Get-Content -LiteralPath $bindingPath -Raw | ConvertFrom-Json
                $identity=(Get-FileHash -LiteralPath $wallet -Algorithm SHA256).Hash
                if(!$binding.wallet_sha256 -or $binding.wallet_sha256 -ne $identity -or $binding.address -notmatch '^qbc1[a-z0-9]+$'){
                    throw 'Active wallet address binding is missing or stale. Unlock / recover address, then verify the backup again.'
                }
                if($backupAddress -cne [string]$binding.address){throw 'This backup belongs to a DIFFERENT wallet. Your active wallet was not changed.'}
                $text+=('Backup verified: opens successfully and matches the active wallet address '+$backupAddress+'.')
            }else{
                $text+=('Backup opens successfully. Address: '+$backupAddress+'. No active wallet exists, so no address comparison was performed.')
            }
        }
        'restore' {
            if(Test-Path -LiteralPath $wallet){throw 'An active wallet exists. It will not be overwritten. Use Verify backup, or restore in a fresh QBTC data folder.'}
            # Native backup decrypts and re-encrypts with protected new-file creation.
            $text=@(& $exe backup ([string]$request.source) $wallet)
            if($LASTEXITCODE -ne 0){throw 'Backup recovery failed; the active wallet was not replaced.'}
            $text+= 'Wallet restored. Use Unlock / recover address once to verify its address before mining.'
        }
        'backup' {
            if (!$request.destination -or (Test-Path -LiteralPath $request.destination)) { throw 'Choose a new backup filename.' }
            $text = @(& $exe backup $wallet ([string]$request.destination))
            if ($LASTEXITCODE -ne 0) { throw 'Backup failed.' }
        }
        'retry' {
            $text=@(Submit-PendingPayment)
        }
        'send' {
            if((Test-Path -LiteralPath $pendingFile) -and !(Resolve-ConfirmedPayment)){throw 'Your previous payment is still unconfirmed. Wait for confirmation; Retry signed payment resubmits only that same payment.'}
            if ($request.address -notmatch '^qbc1[a-z0-9]+$') { throw 'A mainnet recipient is required.' }
            foreach ($field in @('amount','fee')) {
                if ([string]$request.$field -notmatch '^[0-9]{1,16}$') { throw 'Invalid atomic-unit amount.' }
            }
            if ([decimal]$request.amount -le 0) { throw 'Amount must be positive.' }
            $tx = Join-Path $data ('payment-' + [guid]::NewGuid().ToString('N') + '.tx')
            # Reserve this attempt before signing, so a crash cannot hide an ambiguous payment.
            Save-PendingPayment $tx
            try {
                $text = @(& $exe send $wallet $chain ([string]$request.address) ([string]$request.amount) ([string]$request.fee) $tx --mainnet)
                if ($LASTEXITCODE -ne 0) { throw 'Payment was not signed.' }
                Save-PendingPayment $tx
                $text += @(Submit-PendingPayment)
            }catch{
                if(!(Test-Path -LiteralPath $tx)){Remove-Item -LiteralPath $pendingFile -ErrorAction SilentlyContinue}
                throw
            }
        }
        default { throw 'Unknown wallet action.' }
    }
    @{ ok=$true; text=($text -join "`n"); action=$request.action; time=[DateTime]::Now.ToString('HH:mm:ss') } | ConvertTo-Json | Set-Content -LiteralPath $response -Encoding UTF8
} catch {
    @{ ok=$false; text=$_.Exception.Message } | ConvertTo-Json | Set-Content -LiteralPath $response -Encoding UTF8
    Write-Host $_ -ForegroundColor Red
    Read-Host 'Enter to close'
}

finally { if($walletActionLock){$walletActionLock.Dispose()} }
