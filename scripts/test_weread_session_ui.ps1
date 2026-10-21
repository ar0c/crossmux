param([string]$BackendRepo,[switch]$Help)
if($Help){Write-Output 'test_weread_session_ui.ps1 [-BackendRepo path] builds a loopback-only fixture service and exercises real management JavaScript with a DOM seam. Go and Node required. No real secrets, upstream network, uploads or deployment.';exit 0}
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if(!$BackendRepo){$BackendRepo=Join-Path (Split-Path (Split-Path $repo -Parent) -Parent) 'svc/weread-sync'}
$BackendRepo=(Resolve-Path -LiteralPath $BackendRepo).Path
$out=Join-Path $repo ('artifacts/wesync-validation/session-ui/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force -Path $out|Out-Null
$exe=Join-Path $out 'local-acceptance.exe'
Push-Location $BackendRepo
try{ & go build -p=4 -o $exe ./cmd/local-acceptance;if($LASTEXITCODE){throw 'Local fixture build failed'} }finally{Pop-Location}
$process=Start-Process -FilePath $exe -ArgumentList @('-out',('"'+$out+'"')) -WorkingDirectory $BackendRepo -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out 'server.stdout.log') -RedirectStandardError (Join-Path $out 'server.stderr.log')
try{
  $nativeHandle=$process.Handle
  $metadata=Join-Path $out 'local-fixture.json'
  $watch=[Diagnostics.Stopwatch]::StartNew()
  while(!(Test-Path -LiteralPath $metadata)){
    if($process.HasExited){throw 'Local fixture exited before readiness'}
    if($watch.Elapsed.TotalSeconds -gt 10){throw 'Local fixture readiness timeout'}
    Start-Sleep -Milliseconds 150
  }
  & node (Join-Path $BackendRepo 'scripts/test-session-ui.mjs') $metadata
  if($LASTEXITCODE){throw 'Session UI functional acceptance failed'}
  Write-Output "PASS session-ui; report=$(Join-Path $out 'ui-result.json'); browser rendering not verified"
}finally{
  if(!$process.HasExited){Stop-Process -Id $process.Id -Force;$process.WaitForExit()}
  $process.Dispose()
}
