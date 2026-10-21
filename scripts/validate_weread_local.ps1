param(
  [string]$BackendRepo,
  [string]$Suites='service,offline,contract,session-ui,backend',
  [ValidateRange(30,900)][int]$SuiteTimeoutSeconds=300,
  [switch]$Help
)
if($Help){Write-Output 'validate_weread_local.ps1 [-BackendRepo path] [-Suites service,offline,contract,session-ui,backend] [-SuiteTimeoutSeconds 300] runs offline firmware/backend tests and saves logs + result.json. No real credentials, login, uploads or deployments.';exit 0}
$ErrorActionPreference='Stop'
$selectedSuites=@($Suites.Split(',') | ForEach-Object {$_.Trim()})
foreach($name in $selectedSuites){if($name -notin @('service','offline','contract','session-ui','backend')){throw "Unknown suite: $name"}}
if(@($selectedSuites | Select-Object -Unique).Count -ne $selectedSuites.Count){throw 'Duplicate suites are not allowed'}
$repo=Split-Path $PSScriptRoot -Parent
if(!$BackendRepo){$BackendRepo=Join-Path (Split-Path (Split-Path $repo -Parent) -Parent) 'svc/weread-sync'}
$BackendRepo=(Resolve-Path -LiteralPath $BackendRepo).Path
$runId=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$out=Join-Path $repo "artifacts/wesync-validation/runs/$runId"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$psExe=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$results=@()
foreach($suite in $selectedSuites){
  $stdout=Join-Path $out "$suite.stdout.log"
  $stderr=Join-Path $out "$suite.stderr.log"
  $watch=[Diagnostics.Stopwatch]::StartNew()
  if($suite -eq 'backend'){
    $program=(Get-Command go -ErrorAction Stop).Source
    $arguments=@('test','-p=4','-timeout','90s','./...','-json')
    $cwd=$BackendRepo
  }else{
    $program=$psExe
    $name=@{service='test_weread_service.ps1';offline='test_weread_offline.ps1';contract='test_weread_backend_contract.ps1';'session-ui'='test_weread_session_ui.ps1'}[$suite]
    $script=Join-Path $PSScriptRoot $name
    $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$script+'"'))
    if($suite -in @('contract','session-ui')){$arguments+=@('-BackendRepo',('"'+$BackendRepo+'"'))}
    $cwd=$repo
  }
  Write-Output "Running $suite (local fixtures only)"
  $process=Start-Process -FilePath $program -ArgumentList $arguments -WorkingDirectory $cwd -WindowStyle Hidden `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
  try {
    # Retain the native handle: Windows PowerShell may otherwise lose ExitCode
    # after a short-lived redirected child exits.
    $nativeHandle=$process.Handle
    $completed=$process.WaitForExit($SuiteTimeoutSeconds*1000)
    if(!$completed){ & taskkill.exe /PID $process.Id /T /F | Out-Null; $process.WaitForExit(); throw "$suite exceeded its bounded timeout" }
    $code=$process.ExitCode
    $results+=[pscustomobject]@{suite=$suite;exit_code=$code;elapsed_ms=$watch.ElapsedMilliseconds;stdout=$stdout;stderr=$stderr}
    if($code -ne 0){throw "$suite failed; see $stdout and $stderr"}
  }catch{
    [pscustomobject]@{status='failed';error=$_.Exception.Message;results=$results;real_uploads=0}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $out 'result.json') -Encoding UTF8
    throw
  }finally{$process.Dispose()}
}
[pscustomobject]@{status='passed';results=$results;real_uploads=0;real_logins=0;firmware_installations=0;boundary='Host fixtures and production logic; not physical TLS/SD, session coexistence or cloud credit'}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $out 'result.json') -Encoding UTF8
Write-Output "PASS $($results.Count) suites; report=$(Join-Path $out 'result.json')"
