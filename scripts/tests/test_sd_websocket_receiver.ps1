$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot '../upload_sd_websocket.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Upload script has parse errors' }
$function = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Receive-UploadReply'}, $true)
. ([ScriptBlock]::Create($function.Extent.Text))
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Net.WebSockets;
public class UploadFragmentFixture {
  Queue<string> parts;
  public UploadFragmentFixture(string[] data) { parts = new Queue<string>(data); }
  public Task<WebSocketReceiveResult> ReceiveAsync(ArraySegment<byte> buffer, CancellationToken token) {
    var bytes = Encoding.UTF8.GetBytes(parts.Dequeue());
    Array.Copy(bytes, 0, buffer.Array, buffer.Offset, bytes.Length);
    return Task.FromResult(new WebSocketReceiveResult(bytes.Length, WebSocketMessageType.Text, parts.Count == 0));
  }
}
'@
$deadline = [Threading.CancellationTokenSource]::new(5000)
$IdleTimeoutSeconds = 5
try {
  $socket = [UploadFragmentFixture]::new([string[]]@('RE','A','DY'))
  if ((Receive-UploadReply) -ne 'READY') { throw 'Fragmented READY failed' }
  $socket = [UploadFragmentFixture]::new([string[]]@('PROGRESS:', '65536:', '6289648'))
  if ((Receive-UploadReply) -ne 'PROGRESS:65536:6289648') { throw 'Fragmented progress failed' }
  $socket = [UploadFragmentFixture]::new([string[]]@('ERROR:', 'rejected'))
  $failed = $false
  try { Receive-UploadReply | Out-Null } catch { $failed = $_.Exception.Message -eq 'ERROR:rejected' }
  if (-not $failed) { throw 'Server error was not preserved' }
  $socket = [UploadFragmentFixture]::new([string[]]@(('x'*1000), ('x'*24), 'x'))
  $failed = $false
  try { Receive-UploadReply | Out-Null } catch { $failed = $_.Exception.Message -eq 'Oversized WebSocket reply' }
  if (-not $failed) { throw 'Reply size bound failed' }
  'WebSocket reply fragments, error propagation and size bound PASS'
} finally { $deadline.Dispose() }
