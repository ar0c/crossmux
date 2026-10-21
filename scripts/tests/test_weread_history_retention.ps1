$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../weread_history_retention.ps1" -FunctionsOnly
function Assert($condition, $message) { if (-not $condition) { throw $message } }
$files = @(1..112 | ForEach-Object { [pscustomobject]@{name=('weread-time-{0:D4}.json' -f $_); size=100; isDirectory=$false;createdAt=1789450000+$_} })
$files += [pscustomobject]@{name='weread-device-handover.json';size=500;isDirectory=$false}
$files += [pscustomobject]@{name='weread-external-test.bin';size=500;isDirectory=$false}
$files += [pscustomobject]@{name='weread-time-directory.json';size=0;isDirectory=$true}
$files += [pscustomobject]@{name='../weread-time-escape.json';size=500;isDirectory=$false}
$p = Get-HistoryRetentionPlan $files 100 8388608
Assert ($p.Keep.Count -eq 100 -and $p.Remove.Count -eq 12) 'Count cap'
Assert ($p.Keep[0].name -eq 'weread-time-0001.json') 'Original guard retained'
Assert ($p.Bytes -eq 10000) 'Byte accounting'
$p = Get-HistoryRetentionPlan $files 100 250
Assert ($p.Keep.Count -eq 2 -and $p.Bytes -eq 200) 'Byte cap'
Assert ($p.Keep[1].name -eq 'weread-time-0112.json') 'Latest name retained'
$again = Get-HistoryRetentionPlan $p.Keep 100 250
Assert ($again.Remove.Count -eq 0) 'Idempotence'
$p = Get-HistoryRetentionPlan @() 100 8388608
Assert ($p.Keep.Count -eq 0 -and $p.Bytes -eq 0) 'Empty archive'
$blocked = $false
try { Get-HistoryRetentionPlan $files 100 99 | Out-Null } catch { $blocked = $true }
Assert $blocked 'Oversized guard must block deletion'
$p = Get-HistoryRetentionPlan $files 1 100
Assert ($p.Keep.Count -eq 1 -and $p.Remove.Count -eq 111) 'Never delete final guard'
Assert ((Get-HistoryCreatedAt ([pscustomobject]@{ct=1789450000;day='2026-09-13'})) -eq 0) 'Source date is not archive creation time'
Assert ((Get-HistoryCreatedAt ([pscustomobject]@{observed=[pscustomobject]@{observed_at=1789450000}})) -eq 1789450000) 'Observation timestamp'
$files[1].createdAt = 1789459999
$p = Get-HistoryRetentionPlan $files 2 200
Assert ($p.Keep[1].name -eq 'weread-time-0002.json') 'Timestamp must beat filename order'
$files[1].createdAt = 0
$p = Get-HistoryRetentionPlan $files 2 200
Assert ($p.Keep[1].name -eq 'weread-time-0002.json') 'Undated file protected'
$blocked = $false
try { Get-HistoryRetentionPlan $files 1 100 | Out-Null } catch { $blocked = $true }
Assert $blocked 'Undated files cannot be deleted to force budget compliance'
'History retention selection, count/byte bounds, protected files, guard and idempotence PASS'
