param(
  [Parameter(Mandatory=$true)][string]$Journal,
  [Parameter(Mandatory=$true)][string]$Capture,
  [Parameter(Mandatory=$true)][long]$SampledAt,
  [string]$Helper=(Join-Path $PSScriptRoot '../.pio/build/weread-offline/weread_host_bridge.exe')
)
# Offline audit only: no credentials, HTTP, device access or journal writes.
# The caller must supply a capture from the same account as the journal.
$ErrorActionPreference='Stop'
if ($SampledAt -lt 1704067200 -or $SampledAt -gt [uint32]::MaxValue) { throw 'Invalid capture timestamp' }
$journalPath=(Resolve-Path -LiteralPath $Journal).Path
$capturePath=(Resolve-Path -LiteralPath $Capture).Path
$helperPath=(Resolve-Path -LiteralPath $Helper).Path
if ((Get-Item -LiteralPath $journalPath).Length -gt 4MB -or (Get-Item -LiteralPath $capturePath).Length -gt 4MB) {
  throw 'Input exceeds audit bound'
}
$before=(Get-FileHash -LiteralPath $journalPath -Algorithm SHA256).Hash
$stats=Get-Content -LiteralPath $capturePath -Raw | ConvertFrom-Json
if ($null -ne $stats.observed_at -and [long]$stats.observed_at -ne $SampledAt) { throw 'Capture timestamp mismatch' }
$day=[long]([math]::Floor(($SampledAt+28800)/86400)*86400-28800)
$bucket=$stats.readTimes.PSObject.Properties[[string]$day]
if ($null -eq $bucket) { throw 'Day bucket absent; cannot supply complete snapshot' }
$numbers=@($stats.baseTime,$stats.totalReadTime,$bucket.Value)
foreach ($number in $numbers) { if ($null -eq $number -or [string]$number -notmatch '^\d+$') { throw 'Invalid numeric capture' } }
$start=[Diagnostics.ProcessStartInfo]::new($helperPath)
$start.UseShellExecute=$false
$start.CreateNoWindow=$true
$start.RedirectStandardInput=$true
$start.RedirectStandardOutput=$true
$start.RedirectStandardError=$true
foreach ($argument in @('audit',"$SampledAt","$($stats.baseTime)","$day","$($stats.totalReadTime)","$($bucket.Value)")) {
  $start.ArgumentList.Add($argument)
}
$process=[Diagnostics.Process]::Start($start)
try {
  $output=$process.StandardOutput.ReadToEndAsync()
  $errors=$process.StandardError.ReadToEndAsync()
  $bytes=[IO.File]::ReadAllBytes($journalPath)
  $process.StandardInput.BaseStream.Write($bytes,0,$bytes.Length)
  $process.StandardInput.Close()
  if (-not $process.WaitForExit(10000)) { $process.Kill(); throw 'Audit helper timed out' }
  if ($process.ExitCode -ne 0) { throw 'Production codec rejected journal or snapshot' }
  $result=$output.GetAwaiter().GetResult() | ConvertFrom-Json
  if ((Get-FileHash -LiteralPath $journalPath -Algorithm SHA256).Hash -ne $before) { throw 'Journal changed during audit' }
  $result | Add-Member -NotePropertyName journal_hash_unchanged -NotePropertyValue $true
  $result | Add-Member -NotePropertyName cloud_month_seconds -NotePropertyValue $stats.totalReadTime
  $result | Add-Member -NotePropertyName cloud_day_seconds -NotePropertyValue $bucket.Value
  $result | ConvertTo-Json -Compress
} finally { $process.Dispose() }
