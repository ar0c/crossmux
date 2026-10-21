param(
  [Parameter(Mandatory=$true)][string]$LoginDirectory,
  [Parameter(Mandatory=$true)][string]$OutputDirectory
)
# Read-only gateway operation. Credentials are decrypted in memory only.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$authPath = Join-Path $LoginDirectory 'auth.dpapi'
$bytes = [IO.File]::ReadAllBytes($authPath)
$plain = [Security.Cryptography.ProtectedData]::Unprotect($bytes, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
try {
  $auth = [Text.Encoding]::UTF8.GetString($plain) | ConvertFrom-Json
  if ($auth.api_key -notmatch '^wrk-[A-Za-z0-9_-]+$') { throw 'Invalid stored key' }
  $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  $cn = [DateTimeOffset]::FromUnixTimeSeconds($now).ToOffset([TimeSpan]::FromHours(8))
  $month = [DateTimeOffset]::new($cn.Year,$cn.Month,1,0,0,0,[TimeSpan]::FromHours(8)).ToUnixTimeSeconds()
  $body = @{api_name='/readdata/detail';skill_version='1.0.4';mode='monthly';baseTime=$month} | ConvertTo-Json -Compress
  $response = Invoke-WebRequest 'https://i.weread.qq.com/api/agent/gateway' -Method Post -Headers @{Authorization="Bearer $($auth.api_key)";Origin='https://weread.qq.com';Referer='https://weread.qq.com/r/weread-skills'} -ContentType 'application/json' -Body $body -MaximumRedirection 0 -TimeoutSec 30
  $data = $response.Content | ConvertFrom-Json
  if ($null -ne $data.upgrade_info -or ($null -ne $data.errcode -and $data.errcode -ne 0)) { throw 'Gateway rejected read-only query' }
  foreach ($field in @('baseTime','totalReadTime')) {
    if ($null -eq $data.$field -or [string]$data.$field -notmatch '^\d+$') { throw 'Unexpected numeric field' }
  }
  if ($null -eq $data.readTimes -or $data.readTimes -isnot [pscustomobject]) { throw 'Unexpected readTimes shape' }
  $days = [ordered]@{}
  foreach ($property in $data.readTimes.PSObject.Properties) {
    if ($property.Name -notmatch '^\d+$' -or [string]$property.Value -notmatch '^\d+$') { throw 'Unexpected day shape; no raw response saved' }
    $days[$property.Name] = $property.Value
  }
  $dir = New-Item -ItemType Directory -Path $OutputDirectory -Force
  $path = Join-Path $dir.FullName "stats-$now.json"
  if (Test-Path -LiteralPath $path) { throw 'Capture exists' }
  # Only the parser's three numeric fields survive. Never save titles/account/auth.
  $safe = [ordered]@{baseTime=$data.baseTime;totalReadTime=$data.totalReadTime;readTimes=$days}
  [IO.File]::WriteAllText($path, ($safe | ConvertTo-Json -Depth 4 -Compress), [Text.UTF8Encoding]::new($false))
  Write-Output "Read-only capture: $path; sampledAt=$now; HTTP=$($response.StatusCode); dayKeys=$($days.Count); timeReportsSent=0"
} catch {
  # HTTP exception details may contain response bodies; never echo them.
  throw 'Read-only capture failed; no time report sent. Check local login/network or response shape.'
} finally {
  [Array]::Clear($plain,0,$plain.Length)
  $auth=$null
}
