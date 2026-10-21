param(
  [Parameter(Mandatory=$true)][string]$DeviceUrl,
  [Parameter(Mandatory=$true)][string]$LatestFile,
  [Parameter(Mandatory=$true)][string]$BackupDirectory,
  [switch]$Apply
)
$ErrorActionPreference = 'Stop'
$base = $DeviceUrl.TrimEnd('/')
$uri = [Uri]$base
if ($uri.Scheme -ne 'http' -or $uri.AbsolutePath -ne '/' -or $uri.UserInfo -or $uri.Query -or $uri.Fragment) {
  throw 'Use trusted device HTTP origin'
}
$latest = Get-Item -LiteralPath $LatestFile
$pattern = '^\d{6}-\d{6}-[a-f0-9]{8}-ar0c-x4pro\.bin$'
if ($latest.PSIsContainer -or $latest.Name -cnotmatch $pattern) { throw 'Not an ar0c X4 Pro installer' }
$status = Invoke-RestMethod "$base/api/status" -TimeoutSec 10
if ($status.device -ne 'xteink_x4_pro') { throw 'Wrong device model' }
$files = Invoke-RestMethod "$base/api/files?path=/" -TimeoutSec 10
$remote = @($files | Where-Object { $_.name -ceq $latest.Name -and -not $_.isDirectory })
if ($remote.Count -ne 1 -or $remote[0].size -ne $latest.Length) { throw 'Latest installer not present at expected size' }
$old = @($files | Where-Object {
  -not $_.isDirectory -and $_.name -cmatch $pattern -and
  [string]::CompareOrdinal($_.name.Substring(0,13), $latest.Name.Substring(0,13)) -lt 0
})
Write-Output "Latest=$($latest.Name); older installer candidates=$($old.Count)"
if (-not $Apply) { return }
$root = [IO.Path]::GetFullPath($BackupDirectory)
New-Item -ItemType Directory -Path $root -Force | Out-Null
$check = Join-Path $root ($latest.Name + '.' + [Guid]::NewGuid().ToString('N') + '.readback')
Invoke-WebRequest "$base/download?path=/$($latest.Name)" -OutFile $check -TimeoutSec 120
if ((Get-FileHash -LiteralPath $check).Hash -ne (Get-FileHash -LiteralPath $latest.FullName).Hash) {
  throw 'Latest installer readback mismatch; nothing deleted'
}
# Back up ALL old candidates before deleting ANY; never wildcard-delete .bin.
foreach ($file in $old) {
  $copy = Join-Path $root $file.name
  $remoteCopy = Join-Path $root ($file.name + '.' + [Guid]::NewGuid().ToString('N') + '.readback')
  if (-not (Test-Path -LiteralPath $copy)) {
    Invoke-WebRequest "$base/download?path=/$($file.name)" -OutFile $copy -TimeoutSec 120
  }
  Invoke-WebRequest "$base/download?path=/$($file.name)" -OutFile $remoteCopy -TimeoutSec 120
  if ((Get-Item -LiteralPath $copy).Length -ne $file.size -or
      (Get-FileHash -LiteralPath $copy).Hash -ne (Get-FileHash -LiteralPath $remoteCopy).Hash) {
    throw 'Old installer backup mismatch; nothing deleted'
  }
}
foreach ($file in $old) {
  Invoke-RestMethod "$base/delete" -Method Post -Body @{path='/'+$file.name} -TimeoutSec 15 | Out-Null
}
$after = Invoke-RestMethod "$base/api/files?path=/" -TimeoutSec 10
foreach ($file in $old) {
  if (@($after | Where-Object name -CEQ $file.name).Count) { throw 'Old installer still present' }
}
if (@($after | Where-Object name -CEQ $latest.Name).Count -ne 1) { throw 'Latest installer missing' }
Write-Output "Removed $($old.Count) older SD installer packages; verified backups: $root. Running firmware and reading data unchanged."
