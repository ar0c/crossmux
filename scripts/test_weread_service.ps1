param([string]$Compiler='g++',[switch]$Help)
if($Help){Write-Output 'test_weread_service.ps1 [-Compiler g++] runs offline service journal, transport and worker tests; no real uploads.';exit 0}
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$out=Join-Path $repo 'artifacts/wesync-validation'
New-Item -ItemType Directory -Force -Path $out | Out-Null
Push-Location $repo
try {
  $common=@('-std=c++20','-UNDEBUG','-DENABLE_CHINESE_VERSION','-Wall','-Wextra','-Werror','-Ilib/WeReadWebApi/src')
  & $Compiler @common test/weread_webapi/WeReadServiceJournalStandaloneTest.cpp -o "$out/ServiceJournal.exe"
  if($LASTEXITCODE){throw 'Journal compilation failed'}
  & "$out/ServiceJournal.exe"
  if($LASTEXITCODE){throw 'Journal tests failed'}
  & $Compiler @common '-Itest/weread_webapi/time_cloud_stubs' '-Itest/weread_webapi/time_storage_stubs' '-Ilib/JsonParser' `
    test/weread_webapi/WeReadServiceClientStandaloneTest.cpp lib/WeReadWebApi/src/WeReadServiceClient.cpp `
    lib/JsonParser/StreamingJsonParser.cpp -o "$out/ServiceClient.exe"
  if($LASTEXITCODE){throw 'Client compilation failed'}
  & "$out/ServiceClient.exe"
  if($LASTEXITCODE){throw 'Client tests failed'}
  foreach($psram in @($false,$true)) {
    $argsList=@('-std=c++20','-pthread','-UNDEBUG','-DENABLE_CHINESE_VERSION','-Wall','-Wextra','-Werror',
      '-Itest/weread_webapi/time_async_stubs','-Itest/weread_webapi/time_storage_stubs','-Ilib/Memory',
      '-Ilib/WeReadWebApi/src','-Isrc/activities/apps/weread/webapi',
      'test/weread_webapi/WeReadTimeAsyncStandaloneTest.cpp','src/activities/apps/weread/webapi/WeReadTimeSync.cpp')
    if($psram){$argsList+=@('-DBOARD_HAS_PSRAM','-Itest/weread_webapi/time_psram_stubs')}
    $exe=Join-Path $out "ServiceWorker-$psram.exe"
    & $Compiler @argsList -o $exe
    if($LASTEXITCODE){throw 'Worker compilation failed'}
    & $exe
    if($LASTEXITCODE){throw 'Worker tests failed'}
  }
} finally {Pop-Location}
