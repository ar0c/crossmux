param(
  [string]$Capture,
  [long]$SampledAt,
  [ValidateSet('missing','present')][string]$ExpectedDay='missing',
  [string]$Compiler='g++',
  [string]$Python='python'
)
# No firmware build, USB access, network calls or real reports in this runner.
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
Push-Location $repo
try {
  $out=Join-Path $repo '.pio/build/weread-offline'
  New-Item -ItemType Directory -Path $out -Force | Out-Null
  $modules=@('TimeLedger','TimeTransaction','PacedTime','TimeQueue','ExternalTime',
             'HandoverManifest','TimeDataLayout','TimeDiagnostic','TimeStorage',
             'TimeCloud','TimeQuery','TimeReplay','HostBridge')
  $stopwatch=[Diagnostics.Stopwatch]::StartNew()
  foreach ($module in $modules) {
    $argsList=@('-std=c++17','-UNDEBUG','-Wall','-Wextra','-Werror',
      '-Itest/weread_webapi/time_cloud_stubs','-Itest/weread_webapi/time_storage_stubs',
      '-Ilib/WeReadWebApi/src','-Ilib/JsonParser','-Iscripts/diagnostics',"test/weread_webapi/WeRead${module}StandaloneTest.cpp")
    if ($module -in @('TimeCloud','TimeQuery','TimeReplay')) {
      $argsList+=@('lib/WeReadWebApi/src/WeReadTimeResponse.cpp','lib/JsonParser/StreamingJsonParser.cpp')
    }
    if ($module -in @('TimeQuery','TimeReplay')) { $argsList+='lib/WeReadWebApi/src/WeReadTimeCloud.cpp' }
    $exe=Join-Path $out "$module.exe"
    & $Compiler @argsList '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $module" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Regression failed: $module" }
  }
  # Compile the production background worker with real threads and offline HAL/TLS boundaries.
  $asyncArgs=@('-std=c++20','-pthread','-UNDEBUG','-DENABLE_CHINESE_VERSION','-Wall','-Wextra','-Werror',
    '-Itest/weread_webapi/time_async_stubs','-Itest/weread_webapi/time_storage_stubs',
    '-Ilib/Memory','-Ilib/WeReadWebApi/src','-Isrc/activities/apps/weread/webapi',
    'test/weread_webapi/WeReadTimeAsyncStandaloneTest.cpp',
    'src/activities/apps/weread/webapi/WeReadTimeSync.cpp')
  & $Compiler @asyncArgs '-o' (Join-Path $out 'TimeAsync.exe')
  if ($LASTEXITCODE -ne 0) { throw 'Background worker compile failed' }
  & (Join-Path $out 'TimeAsync.exe')
  if ($LASTEXITCODE -ne 0) { throw 'Background worker regression failed' }
  & $Compiler @asyncArgs '-DBOARD_HAS_PSRAM' '-Itest/weread_webapi/time_psram_stubs' '-o' (Join-Path $out 'TimeAsyncPsram.exe')
  if ($LASTEXITCODE -ne 0) { throw 'PSRAM background worker compile failed' }
  & (Join-Path $out 'TimeAsyncPsram.exe')
  if ($LASTEXITCODE -ne 0) { throw 'PSRAM background worker regression failed' }
  # Compile the real POST/payload path with offline platform boundaries too.
  $previousCxx=$env:CXX
  try {
    $env:CXX=$Compiler
    & $Python '-m' 'unittest' 'discover' '-s' 'scripts/tests' '-p' 'test_weread_time_context.py' '-v'
    if ($LASTEXITCODE -ne 0) { throw 'Production time-context regression failed' }
    & $Python '-m' 'unittest' 'discover' '-s' 'scripts/tests' '-p' 'test_weread_verified_network.py' '-v'
    if ($LASTEXITCODE -ne 0) { throw 'Production verified-network regression failed' }
  } finally { $env:CXX=$previousCxx }
  # Native audit helper only; this is not an ESP firmware build.
  & $Compiler '-std=c++17' '-UNDEBUG' '-Wall' '-Wextra' '-Werror' `
    '-Ilib/WeReadWebApi/src' '-Iscripts/diagnostics' `
    'scripts/diagnostics/weread_host_bridge.cpp' '-o' (Join-Path $out 'weread_host_bridge.exe')
  if ($LASTEXITCODE -ne 0) { throw 'Native audit helper compile failed' }
  if ($Capture) {
    if ($SampledAt -le 0) { throw 'Capture requires SampledAt from its read-only capture output' }
    & (Join-Path $out 'TimeReplay.exe') $Capture $SampledAt $ExpectedDay
    if ($LASTEXITCODE -ne 0) { throw 'Private capture replay failed' }
  }
  Write-Output "PASS: $($modules.Count) ledger/cloud suites + background worker + production context/network suites; elapsed=$([math]::Round($stopwatch.Elapsed.TotalSeconds,1))s; real reports=0; firmware builds=0"
} finally { Pop-Location }
