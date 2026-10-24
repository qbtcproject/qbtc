param([string]$Root=(Split-Path $PSScriptRoot),[string]$Store=(Join-Path ([IO.Path]::GetTempPath()) ('qbtc-onion-fixture-'+[guid]::NewGuid().ToString('N'))))
$ErrorActionPreference='Stop'
Add-Type -LiteralPath @((Join-Path $root 'OnionBridge.cs'),(Join-Path $root 'OnionNetwork.cs'))
$path=$Store
$n=New-Object QbtcOnionNetwork(('a'*56+'.onion'),$path,19050,[string[]]@(),'')
try{
 $flags=[Reflection.BindingFlags]'NonPublic,Instance'
 $add=$n.GetType().GetMethod('Add',$flags)
 $source='z'*56+'.onion'
 for($i=0;$i -lt 4;$i++){
  $hostName=([string][char](98+$i))*56+'.onion'
  if(!$add.Invoke($n,@($hostName,$false,$source))){throw 'Candidate refused within budget'}
 }
 if($add.Invoke($n,@(('f'*56+'.onion'),$false,$source))){throw 'Source budget bypass'}
 $book=$n.GetType().GetField('book',$flags).GetValue($n)
 $book.Clear()
 $quotas=$n.GetType().GetField('learned',$flags).GetValue($n)
 $quotas[$source].Until=[DateTime]::UtcNow.AddSeconds(-1)
 if(!$add.Invoke($n,@(('f'*56+'.onion'),$false,$source))){throw 'Expired source budget not renewed'}
 $book.Clear()
 for($i=0;$i -lt 4;$i++){
  $hostName=([string][char](98+$i))*56+'.onion'
  [void]$add.Invoke($n,@($hostName,$false,$null))
  $entry=$book[$hostName];$entry.Due=[DateTime]::MaxValue;$entry.Good=[DateTime]::UtcNow;$entry.FirstGood=[DateTime]::UtcNow
 }
 $peers=@($n.SelectedPeers)
 if($peers.Count -ne 4 -or @($peers | Select-Object -Unique).Count -ne 4){throw 'Synchronization lanes are not distinct'}
 if(($n.SelectedPeers -join ',') -ne ($peers -join ',')){throw 'Healthy lanes changed without reason'}
 $book.Clear()
 for($i=0;$i -lt 64;$i++){
  $name=([string][char](97+($i%26)))*55+[char](99+[int][Math]::Floor($i/26))+'.onion'
  $origin=([string][char](97+($i%26)))*54+'zz'+'.onion'
  [void]$add.Invoke($n,@($name,$false,$origin));$book[$name].Due=[DateTime]::MaxValue
 }
 if($book.Count -ne 64){throw 'Full provenance fixture did not fit'}
 $n.GetType().GetMethod('Save',$flags).Invoke($n,@())
 $n.Dispose()
 $n=New-Object QbtcOnionNetwork(('a'*56+'.onion'),$path,19050,[string[]]@(),'')
 if($n.KnownCount -ne 64){throw ('Full provenance store did not reload: '+$n.LastError)}
 'PASS C# compilation, bounded introductions, rolling source quota renewal and four distinct sticky synchronization lanes'
}finally{$n.Dispose();foreach($file in @($path,($path+'.bak'),($path+'.tmp'))){Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue}}
