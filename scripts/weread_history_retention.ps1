param(
  [string]$DeviceUrl,
  [string]$BackupDirectory,
  [ValidateRange(1, 10000)][int]$MaxFiles = 100,
  [ValidateRange(1, 1073741824)][long]$MaxBytes = 8388608,
  [switch]$Apply,
  [switch]$FunctionsOnly
)
$ErrorActionPreference = 'Stop'

function Get-HistoryCreatedAt {
  param($Document)
  # Only generation/observation timestamps. Source day/ct is NOT file age.
  $times = @($Document.created_at, $Document.generated_at, $Document.observed_at,
             $Document.observed.observed_at, $Document.baseline.observed_at)
  [long]$latest = 0
  foreach ($value in $times) {
    [long]$epoch = 0
    if ([long]::TryParse([string]$value, [ref]$epoch) -and $epoch -ge 1704067200 -and
        $epoch -le [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 300) {
      $latest = [Math]::Max($latest, $epoch)
    }
  }
  return $latest
}

function Remove-VerifiedHistoryComparisons {
  param([string]$Folder)
  $root = [IO.Path]::GetFullPath($Folder)
  $prefix = $root.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
  foreach ($copy in Get-ChildItem -LiteralPath $root -File) {
    if ($copy.Name -cnotmatch '^(weread-(?:time|backlog)-[A-Za-z0-9_-]+\.json)\.[a-f0-9]{32}\.verify$') { continue }
    $original = Join-Path $root $Matches[1]
    if (-not $copy.FullName.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $original -PathType Leaf)) { continue }
    if ((Get-FileHash -LiteralPath $copy.FullName).Hash -eq (Get-FileHash -LiteralPath $original).Hash) {
      Remove-Item -LiteralPath $copy.FullName
      Write-Output "Removed redundant verified comparison: $($copy.Name); original retained"
    }
  }
}

function Get-HistoryRetentionPlan {
  param([object[]]$Files, [int]$CountLimit, [long]$ByteLimit)
  if ($CountLimit -lt 1 -or $ByteLimit -lt 1) { throw 'Invalid retention limits' }
  # The first original name remains the guard, independent of timestamp order.
  $eligible = @($Files | Where-Object {
    -not $_.isDirectory -and $_.name -cmatch '^weread-(time|backlog)-[A-Za-z0-9_-]+\.json$'
  } | Sort-Object name)
  if (@($eligible.name | Sort-Object -Unique).Count -ne $eligible.Count) { throw 'Duplicate archive names' }
  foreach ($item in $eligible) {
    if ($null -eq $item.size -or [long]$item.size -lt 0) { throw 'Invalid archive size' }
  }
  $keep = [Collections.Generic.List[object]]::new()
  $remove = [Collections.Generic.List[object]]::new()
  [long]$bytes = 0
  if ($eligible.Count) {
    # Never remove the final original marker: missing-receipt protection relies
    # on its existence, even when all other historical JSON has been archived.
    $guard = $eligible[0]
    $protected = @($eligible | Where-Object { $_.name -ceq $guard.name -or -not $_.createdAt })
    foreach ($item in $protected) { $keep.Add($item); $bytes += [long]$item.size }
    if ($bytes -gt $ByteLimit -or $keep.Count -gt $CountLimit) {
      throw 'Protected or undated history exceeds budget; no files deleted; inspect archive metadata'
    }
    $dated = @($eligible | Where-Object { $_.name -cne $guard.name -and $_.createdAt } |
      Sort-Object @{Expression='createdAt';Descending=$true}, @{Expression='name';Descending=$true})
    foreach ($item in $dated) {
      if ($keep.Count -lt $CountLimit -and [long]$item.size -le ($ByteLimit - $bytes)) {
        $keep.Add($item)
        $bytes += [long]$item.size
      } else { $remove.Add($item) }
    }
  }
  [pscustomobject]@{ Keep=$keep.ToArray(); Remove=$remove.ToArray(); Bytes=$bytes }
}

if ($FunctionsOnly) { return }
if (-not $DeviceUrl -or -not $BackupDirectory) { throw 'DeviceUrl and BackupDirectory are required' }
$base = $DeviceUrl.TrimEnd('/')
$origin = [Uri]$base
if ($origin.Scheme -ne 'http' -or $origin.AbsolutePath -ne '/' -or $origin.UserInfo -or $origin.Query -or $origin.Fragment) {
  throw 'Use the trusted device HTTP origin only'
}
$backupRoot = [IO.Path]::GetFullPath($BackupDirectory)
$backupPrefix = $backupRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$files = Invoke-RestMethod "$base/api/files?path=/WeReadSync/history" -TimeoutSec 10
foreach ($file in $files) {
  if ($file.isDirectory -or $file.name -cnotmatch '^weread-(time|backlog)-[A-Za-z0-9_-]+\.json$') { continue }
  [long]$createdAt = 0
  # Bounded metadata parse; malformed/oversized/undated files are protected.
  if ([long]$file.size -gt 0 -and [long]$file.size -le 1048576) {
    $path = '/WeReadSync/history/' + $file.name
    $response = Invoke-WebRequest "$base/download?path=$([Uri]::EscapeDataString($path))" -TimeoutSec 20
    $json = if ($response.Content -is [byte[]]) { [Text.Encoding]::UTF8.GetString($response.Content) } else { [string]$response.Content }
    try { $createdAt = Get-HistoryCreatedAt ($json | ConvertFrom-Json -ErrorAction Stop) }
    catch { Write-Warning "No reliable timestamp: $($file.name); protected from deletion" }
  }
  $file | Add-Member -NotePropertyName createdAt -NotePropertyValue $createdAt -Force
}
$plan = Get-HistoryRetentionPlan -Files $files -CountLimit $MaxFiles -ByteLimit $MaxBytes
Write-Output "History retention: keep $($plan.Keep.Count) files / $($plan.Bytes) bytes; remove $($plan.Remove.Count); limits $MaxFiles files / $MaxBytes bytes"
if (-not $Apply) { return }
New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
Remove-VerifiedHistoryComparisons $backupRoot
if (-not $plan.Remove.Count) { return }
$verified = @()
# Back up and re-read every removal candidate before performing ANY deletion.
foreach ($file in $plan.Remove) {
  $local = [IO.Path]::GetFullPath((Join-Path $backupRoot $file.name))
  if (-not $local.StartsWith($backupPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Backup path escapes target' }
  $remote = '/WeReadSync/history/' + $file.name
  $url = "$base/download?path=$([Uri]::EscapeDataString($remote))"
  if (-not (Test-Path -LiteralPath $local)) { Invoke-WebRequest $url -OutFile $local -TimeoutSec 20 }
  $check = Join-Path $backupRoot ($file.name + '.' + [Guid]::NewGuid().ToString('N') + '.verify')
  Invoke-WebRequest $url -OutFile $check -TimeoutSec 20
  $hash = (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash
  if ((Get-Item -LiteralPath $local).Length -ne [long]$file.size -or
      (Get-FileHash -LiteralPath $check -Algorithm SHA256).Hash -ne $hash) { throw 'Backup differs from device; no further changes' }
  # Only this exact, task-created comparison copy is removed. Keep the backup.
  if (-not [IO.Path]::GetFullPath($check).StartsWith($backupPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Verification path escapes target'
  }
  Remove-Item -LiteralPath $check
  $verified += [pscustomobject]@{ Path=$remote; Hash=$hash; Local=$local; Size=[long]$file.size }
}
foreach ($file in $verified) {
  # Only immutable archived diagnostic JSON is in this list; no ledger or receipt.
  Invoke-RestMethod "$base/delete" -Method Post -Body @{path=$file.Path} -TimeoutSec 15 | Out-Null
}
$after = Invoke-RestMethod "$base/api/files?path=/WeReadSync/history" -TimeoutSec 10
foreach ($item in $after) {
  $known = @($plan.Keep | Where-Object name -CEQ $item.name)
  if ($known.Count -eq 1) { $item | Add-Member -NotePropertyName createdAt -NotePropertyValue $known[0].createdAt -Force }
}
foreach ($file in $verified) {
  if (@($after | Where-Object { '/WeReadSync/history/' + $_.name -ceq $file.Path }).Count) { throw 'Deletion not confirmed' }
}
foreach ($file in $plan.Keep) {
  if (@($after | Where-Object { $_.name -ceq $file.name -and $_.size -eq $file.size -and -not $_.isDirectory }).Count -ne 1) {
    throw 'Retained archive changed; stop'
  }
}
$final = Get-HistoryRetentionPlan -Files $after -CountLimit $MaxFiles -ByteLimit $MaxBytes
if ($final.Remove.Count) { throw 'Archive changed concurrently; rerun a dry-run, do not delete blindly' }
Write-Output "Verified: removed $($verified.Count), retained $($final.Keep.Count), bytes $($final.Bytes). Recoverable originals: $backupRoot"
