param([Parameter(Mandatory=$true)][string]$DeviceUrl)
$ErrorActionPreference = 'Stop'
function ConvertTo-SafeDiagnostic {
  param($Status, $Diagnostic)
  $stages = @('none','startup_memory','startup_session','startup_audit','identity','login','reader_id',
    'reader_request','reader_signature','progress_memory','progress_request','progress_payload','toc_open',
    'toc_read','chapter_missing','progress_range','progress_fraction','session_save','clock','cloud_key',
    'cloud_stats','enter','report')
  if ($Diagnostic.schema -notin @(1,2,3) -or $stages -cnotcontains [string]$Diagnostic.stage) {
    throw 'Unsupported diagnostic record'
  }
  if ([string]$Status.version -cnotmatch '^[A-Za-z0-9._+/-]{1,80}$' -or
      [string]$Status.device -cnotmatch '^[a-z0-9_]{1,40}$') {
    throw 'Invalid device status'
  }
  $summary = [ordered]@{ version=[string]$Status.version; device=[string]$Status.device; stage=[string]$Diagnostic.stage }
  foreach ($pair in @(
      @('uptime', $Status.uptime), @('error', $Diagnostic.error), @('http', $Diagnostic.http),
      @('detail', $Diagnostic.detail), @('queue', $Diagnostic.queue), @('phase', $Diagnostic.phase),
      @('issue', $Diagnostic.issue), @('recordedUptimeMs', $Diagnostic.uptime_ms))) {
    if ($null -eq $pair[1] -or [string]$pair[1] -cnotmatch '^-?\d{1,10}$') { throw "Invalid diagnostic field: $($pair[0])" }
    $summary[$pair[0]] = [long]$pair[1]
  }
  $report = $null
  if ($Diagnostic.schema -ge 2) {
    $report = [ordered]@{}
    foreach ($field in @('attempted','accepted','seconds','prepare_age_ms','entry_gap_ms',
        'has_previous','previous_gap_ms','elapsed_ms','response_bytes','http','error','guard','succ','synckey')) {
      $value = $Diagnostic.report.$field
      if ($null -eq $value -or [string]$value -cnotmatch '^-?\d{1,10}$') { throw "Invalid diagnostic field: $field" }
      $report[$field] = [long]$value
    }
  }
  $network = $null
  if ($Diagnostic.schema -eq 3) {
    $network = [ordered]@{}
    foreach ($field in @('stage','error','socket','tls','verify','elapsed_ms')) {
      $value = $Diagnostic.network.$field
      if ($null -eq $value -or [string]$value -cnotmatch '^-?\d{1,10}$') { throw "Invalid network field: $field" }
      $network[$field] = [long]$value
    }
    if ([string]$Diagnostic.retries -cnotmatch '^[0-3]$') { throw 'Invalid retry count' }
    $network.retries = [int]$Diagnostic.retries
  }
  # Publish only after every field has passed validation; never emit a partial record.
  [pscustomobject]@{ Summary=[pscustomobject]$summary; Report=$report; Network=$network }
}
$base = $DeviceUrl.TrimEnd('/')
$origin = [Uri]$base
if ($origin.Scheme -ne 'http' -or $origin.AbsolutePath -ne '/' -or $origin.UserInfo -or $origin.Query -or $origin.Fragment) {
  throw 'Use the trusted device HTTP origin only'
}
$status = Invoke-RestMethod "$base/api/status" -TimeoutSec 10
$response = Invoke-WebRequest "$base/download?path=/WeReadSync/last-sync-diagnostic.json" -TimeoutSec 10
$json = if ($response.Content -is [byte[]]) { [Text.Encoding]::UTF8.GetString($response.Content) } else { [string]$response.Content }
if ($json.Length -gt 768) { throw 'Diagnostic record exceeds schema bound' }
$d = $json | ConvertFrom-Json
$safe = ConvertTo-SafeDiagnostic -Status $status -Diagnostic $d
$safe.Summary | ConvertTo-Json
if ($null -ne $safe.Report) { [pscustomobject]$safe.Report | ConvertTo-Json }
if ($null -ne $safe.Network) { [pscustomobject]$safe.Network | ConvertTo-Json }
