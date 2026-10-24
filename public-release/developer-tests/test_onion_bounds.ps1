param([string]$Root=(Split-Path $PSScriptRoot))
$ErrorActionPreference='Stop'
Add-Type -Path @((Join-Path $Root 'OnionBridge.cs'),(Join-Path $Root 'OnionNetwork.cs'))
$store=Join-Path ([IO.Path]::GetTempPath()) ('qbtc-onion-bounds-'+[guid]::NewGuid()+'.txt')
$own=('a'*56)+'.onion'
$m=New-Object QbtcOnionNetwork($own,$store,19049,[string[]]@(),'')
$method=[QbtcOnionNetwork].GetMethod('Add',[Reflection.BindingFlags]'Instance,NonPublic')
$script:checks=0
function Need($test,$label){if(!$test){throw $label};$script:checks++;Write-Output ('PASS '+$label)}
function Add($name,$seed,$source){return $method.Invoke($m,[object[]]@($name,[bool]$seed,$source))}
try{
 $listener=[QbtcOnionNetwork].GetField('listener',[Reflection.BindingFlags]'Instance,NonPublic').GetValue($m)
 Need ($listener.ExclusiveAddressUse) 'discovery listener reserves port exclusively'
 $bridge=[QbtcOnionNetwork].GetField('bridge',[Reflection.BindingFlags]'Instance,NonPublic').GetValue($m)
 $native=[QbtcOnionBridge].GetField('listener',[Reflection.BindingFlags]'Instance,NonPublic').GetValue($bridge)
 Need ($native.ExclusiveAddressUse) 'bridge listener reserves port exclusively'
 Need (!(Add $own $false $null)) 'self address excluded'
 Need (!(Add '127.0.0.1' $false $null)) 'numeric address refused'
 Need (!(Add ('https://'+('b'*56)+'.onion') $false $null)) 'URL prefix refused'
 Need (!(Add (('b'*56)+'.onion:19121') $false $null)) 'control port / arbitrary port refused'
 Need (!(Add (('b'*55)+'.onion') $false $null)) 'wrong onion length refused'
 $names=@('b','c','d','e','f') | ForEach-Object {($_*56)+'.onion'}
 foreach($name in $names[0..3]){Need (Add $name $false 'one-source') 'bounded source candidate accepted'}
 Need (!(Add $names[4] $false 'one-source')) 'one source cannot add a fifth candidate'
 Need (!(Add $names[0] $false $null)) 'duplicate is not added'
 for($i=0;$i -lt 100;$i++){
  $suffix='';$v=$i
  for($j=0;$j -lt 3;$j++){$suffix+='abcdefghijklmnopqrstuvwxyz234567'[$v%32];$v=[int][Math]::Floor($v/32)}
  $null=Add (('g'*53)+$suffix+'.onion') $false $null
 }
 Need ($m.KnownCount -eq 64) 'address book stays capped at 64'
}finally{$m.Dispose();Remove-Item $store,($store+'.tmp'),($store+'.bak') -ErrorAction SilentlyContinue}
Write-Output ('PASS '+$script:checks+' onion syntax, source-budget and capacity checks')
