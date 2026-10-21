param(
  [Parameter(Mandatory = $true)][string]$DeviceUrl,
  [Parameter(Mandatory = $true)][string]$BackupDirectory,
  [switch]$Apply
)
$ErrorActionPreference = 'Stop'
$base = $DeviceUrl.TrimEnd('/')
$uri = [Uri]$base
if ($uri.Scheme -ne 'http' -or $uri.AbsolutePath -ne '/' -or $uri.UserInfo) {
  throw 'Use the trusted device HTTP origin, without credentials or a path'
}
$backup = [IO.Path]::GetFullPath($BackupDirectory)
$files = Invoke-RestMethod "$base/api/files?path=/" -TimeoutSec 10
# Only historical diagnostic JSON, never the live manifest, binary receipts,
# native journals, credentials, books, firmware or sleep artwork.
$selected = @($files | Where-Object {
  -not $_.isDirectory -and $_.name -cmatch '^weread-(time|backlog)-[A-Za-z0-9_-]+\.json$'
})
Write-Output "Historical audit JSON selected: $($selected.Count); active handover/receipts remain unchanged"
if (-not $Apply) { return }
New-Item -ItemType Directory -Path $backup -Force | Out-Null
foreach ($folder in @('WeReadSync', 'WeReadSync/history')) {
  $parent = if ($folder.Contains('/')) { '/WeReadSync' } else { '/' }
  $leaf = $folder.Split('/')[-1]
  $siblings = Invoke-RestMethod "$base/api/files?path=$parent" -TimeoutSec 10
  $existing = @($siblings | Where-Object { $_.name -ceq $leaf })
  if ($existing.Count) {
    if ($existing.Count -ne 1 -or -not $existing[0].isDirectory) { throw 'Destination directory conflict' }
  } else {
    Invoke-RestMethod "$base/mkdir" -Method Post -Body @{ name=$leaf; path=$parent } -TimeoutSec 10 | Out-Null
  }
}
$destFiles = Invoke-RestMethod "$base/api/files?path=/WeReadSync/history" -TimeoutSec 10
$manifest = @()
$manifestPath = Join-Path $backup 'manifest.json'
$prior = if (Test-Path -LiteralPath $manifestPath) { Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json } else { @() }
# Download every selected source before the first move; never overwrite backups.
foreach ($file in $selected) {
  if (@($destFiles | Where-Object { $_.name -ceq $file.name }).Count) { throw 'Destination file already exists' }
  $local = Join-Path $backup $file.name
  if (Test-Path -LiteralPath $local) {
    $saved = @($prior | Where-Object { $_.name -ceq $file.name })
    if ($saved.Count -ne 1 -or (Get-FileHash -LiteralPath $local).Hash -ne $saved[0].sha256) {
      throw 'Unverified backup already exists; use a new backup directory'
    }
  } else {
    Invoke-WebRequest "$base/download?path=$([Uri]::EscapeDataString('/'+$file.name))" -OutFile $local -TimeoutSec 15
  }
  if ((Get-Item -LiteralPath $local).Length -ne $file.size) { throw 'Backup length mismatch' }
  $manifest += [pscustomobject]@{ name=$file.name; bytes=$file.size; sha256=(Get-FileHash -LiteralPath $local).Hash }
}
if (-not (Test-Path -LiteralPath $manifestPath)) {
  $manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $manifestPath -Encoding utf8
}
$count = 0
foreach ($file in $manifest) {
  $old = '/' + $file.name
  $new = '/WeReadSync/history/' + $file.name
  # WebDAV MOVE is a same-card rename; Overwrite:F prevents destructive replacement.
  Invoke-WebRequest "$base$old" -CustomMethod MOVE -Headers @{Destination="$base$new"; Overwrite='F'} -TimeoutSec 15 | Out-Null
  $readback = Join-Path $backup ($file.name + '.readback')
  Invoke-WebRequest "$base/download?path=$([Uri]::EscapeDataString($new))" -OutFile $readback -TimeoutSec 15
  if ((Get-FileHash -LiteralPath $readback).Hash -ne $file.sha256) { throw 'Moved file checksum mismatch; stop' }
  $count++
  if ($count % 10 -eq 0) { Write-Output "Moved and verified $count/$($manifest.Count)" }
}
$root = Invoke-RestMethod "$base/api/files?path=/" -TimeoutSec 10
foreach ($file in $manifest) {
  if (@($root | Where-Object { $_.name -ceq $file.name }).Count) { throw 'Original unexpectedly remains in root' }
}
Write-Output "Completed: $count audit files moved and SHA-256 verified; backups: $backup"
& "$PSScriptRoot/weread_history_retention.ps1" -DeviceUrl $base -BackupDirectory (Join-Path $backup 'retention') -Apply
