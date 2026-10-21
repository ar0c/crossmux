param(
  [Parameter(Mandatory=$true)][string]$DeviceHost,
  [Parameter(Mandatory=$true)][string]$File,
  [string]$Directory = '/',
  [ValidateRange(5, 300)][int]$IdleTimeoutSeconds = 30,
  [switch]$AllowOverwrite,
  [switch]$KeepReadback
)
$ErrorActionPreference = 'Stop'
$item = Get-Item -LiteralPath $File
if ($item.PSIsContainer -or $item.Name.Contains(':') -or $Directory.Contains(':')) { throw 'Invalid upload target' }
if ($DeviceHost -notmatch '^[A-Za-z0-9.-]+$' -or -not $Directory.StartsWith('/') -or
    $Directory.Contains('\') -or @($Directory.Split('/') | Where-Object { $_ -eq '.' -or $_ -eq '..' }).Count) {
  throw 'Use a device hostname/IPv4 address and an absolute SD directory without traversal'
}
$base = "http://$DeviceHost"
$remote = $Directory.TrimEnd('/') + '/' + $item.Name
$listing = Invoke-RestMethod "$base/api/files?path=$([Uri]::EscapeDataString($Directory))" -TimeoutSec $IdleTimeoutSeconds
$existing = @($listing | Where-Object { $_.name -ieq $item.Name })
if ($existing.Count -and (-not $AllowOverwrite -or $existing[0].isDirectory)) {
  throw 'Destination exists; no upload attempted (explicit AllowOverwrite required for files)'
}
$socket = [Net.WebSockets.ClientWebSocket]::new()
$socket.Options.Proxy = $null
$deadline = [Threading.CancellationTokenSource]::new()
$stream = $null
function Receive-UploadReply {
  $deadline.CancelAfter($IdleTimeoutSeconds * 1000)
  $buffer = [byte[]]::new(1024)
  $used = 0
  do {
    if ($used -eq $buffer.Length) { throw 'Oversized WebSocket reply' }
    $result = $socket.ReceiveAsync([ArraySegment[byte]]::new($buffer, $used, $buffer.Length-$used), $deadline.Token).GetAwaiter().GetResult()
    if ($result.MessageType -ne [Net.WebSockets.WebSocketMessageType]::Text) {
      throw "Unexpected WebSocket reply type: $($result.MessageType); status: $($result.CloseStatus)"
    }
    $used += $result.Count
  } while (-not $result.EndOfMessage)
  $text = [Text.Encoding]::UTF8.GetString($buffer, 0, $used)
  if ($text.StartsWith('ERROR:')) { throw $text }
  return $text
}
try {
  # Hold a read-only shared handle: local replacement/writes cannot change the
  # bytes between hashing, transmission and remote verification.
  $stream = [IO.File]::Open($item.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
  $sourceHash = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash
  $stream.Position = 0
  if ($stream.Length -ne $item.Length) { throw 'Source length changed before upload' }
  $deadline.CancelAfter($IdleTimeoutSeconds * 1000)
  $null = $socket.ConnectAsync([Uri]"ws://${DeviceHost}:81/", $deadline.Token).GetAwaiter().GetResult()
  $start = [Text.Encoding]::UTF8.GetBytes("START:$($item.Name):$($item.Length):$Directory")
  $null = $socket.SendAsync([ArraySegment[byte]]::new($start), [Net.WebSockets.WebSocketMessageType]::Text, $true, $deadline.Token).GetAwaiter().GetResult()
  if ((Receive-UploadReply) -ne 'READY') { throw 'Device did not acknowledge upload start' }
  $chunk = [byte[]]::new(4096)
  [long]$sent = 0
  while (($count = $stream.Read($chunk, 0, $chunk.Length)) -gt 0) {
    $deadline.CancelAfter($IdleTimeoutSeconds * 1000)
    $null = $socket.SendAsync([ArraySegment[byte]]::new($chunk, 0, $count), [Net.WebSockets.WebSocketMessageType]::Binary, $true, $deadline.Token).GetAwaiter().GetResult()
    $sent += $count
    if ($sent % 65536 -eq 0 -and $sent -lt $item.Length) {
      $reply = Receive-UploadReply
      if ($reply -ne "PROGRESS:${sent}:$($item.Length)") { throw 'Unexpected progress acknowledgement' }
    }
    if ($sent % 524288 -eq 0) { Write-Output "Uploaded $sent/$($item.Length) bytes" }
  }
  $reply = Receive-UploadReply
  if ($reply.StartsWith('PROGRESS:')) {
    if ($reply -ne "PROGRESS:$($item.Length):$($item.Length)") { throw 'Invalid final progress acknowledgement' }
    $reply = Receive-UploadReply
  }
  if ($reply -ne 'DONE') { throw 'Upload completion not acknowledged' }
  $socket.Dispose()
  # DONE is not acceptance: independently download all bytes with an idle
  # timeout per read, not a fixed deadline for the entire large transfer.
  $readbackPath = Join-Path $item.DirectoryName ($item.Name + '.' + [Guid]::NewGuid().ToString('N') + '.readback')
  $handler = [Net.Http.HttpClientHandler]::new()
  $handler.UseProxy = $false
  $client = [Net.Http.HttpClient]::new($handler)
  $client.Timeout = [Threading.Timeout]::InfiniteTimeSpan
  $response = $null; $input = $null; $output = $null
  try {
    $deadline.CancelAfter($IdleTimeoutSeconds * 1000)
    $response = $client.GetAsync("$base/download?path=$([Uri]::EscapeDataString($remote))",
      [Net.Http.HttpCompletionOption]::ResponseHeadersRead, $deadline.Token).GetAwaiter().GetResult()
    $null = $response.EnsureSuccessStatusCode()
    $input = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
    $output = [IO.File]::Open($readbackPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $buffer = [byte[]]::new(65536)
    [long]$received = 0
    do {
      $deadline.CancelAfter($IdleTimeoutSeconds * 1000)
      $n = $input.ReadAsync($buffer, 0, $buffer.Length, $deadline.Token).GetAwaiter().GetResult()
      if ($n -gt 0) {
        $received += $n
        if ($received -gt $item.Length) { throw 'Readback exceeds source length' }
        $output.Write($buffer, 0, $n)
      }
    } while ($n -gt 0)
  } finally {
    if ($output) { $output.Dispose() }
    if ($input) { $input.Dispose() }
    if ($response) { $response.Dispose() }
    $client.Dispose()
  }
  if ($received -ne $item.Length -or (Get-FileHash -LiteralPath $readbackPath -Algorithm SHA256).Hash -ne $sourceHash) {
    throw "Upload readback mismatch; not verified. Evidence: $readbackPath"
  }
  if ($KeepReadback) {
    Write-Output "Readback evidence retained: $readbackPath"
  } else {
    $parent = [IO.Path]::GetFullPath($item.DirectoryName).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not [IO.Path]::GetFullPath($readbackPath).StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)) {
      throw 'Readback cleanup path escapes source directory'
    }
    Remove-Item -LiteralPath $readbackPath
  }
  Write-Output "Upload and independent readback VERIFIED: $received bytes; SHA256=$sourceHash"
} finally {
  if ($stream) { $stream.Dispose() }
  $socket.Dispose()
  $deadline.Dispose()
}
