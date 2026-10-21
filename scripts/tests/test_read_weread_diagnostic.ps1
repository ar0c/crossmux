$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot '../read_weread_diagnostic.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Diagnostic script has parse errors' }
$function = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and
  $n.Name -eq 'ConvertTo-SafeDiagnostic'}, $true)
if (-not $function) { throw 'Diagnostic validation function missing' }
. ([ScriptBlock]::Create($function.Extent.Text))

$status = [pscustomobject]@{version='1.5.8-dev-codex/test-a4d18098';device='xteink_x4_pro';uptime=12}
$diagnostic = [pscustomobject]@{
  schema=3;stage='report';error=0;http=200;detail=0;queue=1;phase=2;issue=0;uptime_ms=12345;retries=0
  report=[pscustomobject]@{attempted=1;accepted=1;seconds=30;prepare_age_ms=10;entry_gap_ms=20;
    has_previous=0;previous_gap_ms=0;elapsed_ms=50;response_bytes=60;http=200;error=0;guard=0;succ=1;synckey=1}
  network=[pscustomobject]@{stage=1;error=0;socket=0;tls=0;verify=0;elapsed_ms=50}
}
function Assert-RejectedWithoutOutput($candidate) {
  $captured = @()
  $rejected = $false
  try { $captured = @(ConvertTo-SafeDiagnostic -Status $status -Diagnostic $candidate) }
  catch { $rejected = $true }
  if (-not $rejected -or $captured.Count) { throw 'Unsafe diagnostic emitted or accepted' }
}
$safe = ConvertTo-SafeDiagnostic -Status $status -Diagnostic $diagnostic
if ($safe.Summary.stage -ne 'report' -or $safe.Summary.error -ne 0 -or
    $safe.Report.seconds -ne 30 -or $safe.Network.retries -ne 0) { throw 'Valid diagnostic rejected' }

foreach ($field in @('error','http','detail','queue','phase','issue','uptime_ms')) {
  $candidate = $diagnostic | ConvertTo-Json -Depth 5 | ConvertFrom-Json
  $candidate.$field = 'private-value'
  Assert-RejectedWithoutOutput $candidate
}
$candidate = $diagnostic | ConvertTo-Json -Depth 5 | ConvertFrom-Json
$candidate.stage = 'private_value'
Assert-RejectedWithoutOutput $candidate
$candidate = $diagnostic | ConvertTo-Json -Depth 5 | ConvertFrom-Json
$candidate.network.error = 'private-value'
Assert-RejectedWithoutOutput $candidate
$candidate = $diagnostic | ConvertTo-Json -Depth 5 | ConvertFrom-Json
$candidate.report.seconds = 'private-value'
Assert-RejectedWithoutOutput $candidate
'Diagnostic rejects unsafe root, report and network fields before output PASS'
