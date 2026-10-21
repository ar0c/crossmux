param([string]$BackendRepo, [string]$Compiler='g++', [switch]$Help)
if($Help){Write-Output 'test_weread_backend_contract.ps1 [-BackendRepo path] [-Compiler g++] runs real device/client and backend/handler contract tests offline; no real credentials or uploads.';exit 0}
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if(!$BackendRepo){$BackendRepo=Join-Path (Split-Path (Split-Path $repo -Parent) -Parent) 'svc/weread-sync'}
if(!(Test-Path -LiteralPath (Join-Path $BackendRepo 'internal/server/device_contract_test.go'))){throw 'Matching backend contract test is missing; provide -BackendRepo'}
$out=Join-Path $repo 'artifacts/wesync-validation'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe=Join-Path $out 'DeviceBackendBridge.exe'
$previous=$env:WESYNC_DEVICE_CONTRACT_EXE
Push-Location $repo
try {
  & $Compiler '-std=c++20' '-UNDEBUG' '-DENABLE_CHINESE_VERSION' '-Wall' '-Wextra' '-Werror' `
    '-Ilib/WeReadWebApi/src' '-Itest/weread_webapi/time_cloud_stubs' '-Itest/weread_webapi/time_storage_stubs' '-Ilib/JsonParser' '-Ifreeink-sdk/libs/network/JsonSax/include' `
    'test/weread_webapi/WeReadServiceBackendBridge.cpp' 'lib/WeReadWebApi/src/WeReadServiceClient.cpp' `
    'freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp' -o $exe
  if($LASTEXITCODE){throw 'Device bridge compilation failed'}
  $env:WESYNC_DEVICE_CONTRACT_EXE=$exe
  Push-Location $BackendRepo
  try { & go test -p=4 ./internal/server -run '^TestProductionDeviceContract$' -count=1 -timeout 45s -v
    if($LASTEXITCODE){throw 'Cross-language contract failed'}
  } finally {Pop-Location}
} finally {$env:WESYNC_DEVICE_CONTRACT_EXE=$previous;Pop-Location}
