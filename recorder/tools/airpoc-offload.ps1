<#
airpoc-offload.ps1 — pull recordings off the Jetson onto this Windows box.

Runs from PowerShell with native OpenSSH (ssh.exe/scp.exe) and tar.exe (both
ship with Windows 10/11) — no WSL required. Tiers let you grab the light stuff
(annotations, thumbnails, radar, metadata) fast and pull the heavy native video
only when you want it.

  # everything except raw native, for every saved session:
  .\airpoc-offload.ps1 -RemoteHost asaftg@orin-nano -Tier display -Dest D:\airpoc

  # one session, full (incl. raw native Y10):
  .\airpoc-offload.ps1 -RemoteHost asaftg@orin-nano -Sid 20260705T070724Z -Tier full

  # full pull, then free the raw channel on the Jetson once it's safely local:
  .\airpoc-offload.ps1 -RemoteHost asaftg@orin-nano -Tier full -PruneNativeAfter

Tiers:  meta    = manifest + thumbs + radar + events + stats (MBs)
        display = meta + the display video the operator saw (~0.2-1 GB/session)
        full    = display + raw native Y10 (7-37 GB/session)

Binary-safe by construction: we build the tar ON the Jetson, scp it (scp handles
binary; PowerShell pipes do not), extract locally, then delete the remote tar.
For very large native pulls over flaky WiFi, the resumable path is the bash tool
tools/offload_pull.sh run under WSL (rsync --partial); this script is the
zero-setup native-Windows option.
#>
param(
  [string]$RemoteHost = "asaftg@orin-nano",
  [ValidateSet("meta","display","full")][string]$Tier = "display",
  [string]$Dest = ".\recordings",
  [string]$Sid = "",                       # empty = all saved sessions
  [string]$RemoteRoot = "/data/recordings",
  [string]$RecorderPort = "8093",
  [switch]$PruneNativeAfter
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# per-tier tar excludes (paths relative to the session dir)
$excl = switch ($Tier) {
  "meta"    { @("--exclude=*/eo_y10", "--exclude=*/eo_jpeg") }
  "display" { @("--exclude=*/eo_y10") }
  "full"    { @() }
}
$exclArg = ($excl -join " ")

# session list
if ($Sid) { $sids = @($Sid) }
else {
  $sids = (ssh $RemoteHost "ls -1 $RemoteRoot 2>/dev/null | grep -E '^[0-9]{8}T[0-9]{6}Z$'") -split "`n" |
          ForEach-Object { $_.Trim() } | Where-Object { $_ }
}
if (-not $sids) { Write-Host "no sessions found on $RemoteHost"; exit 1 }
Write-Host "offloading $($sids.Count) session(s), tier=$Tier -> $Dest"

foreach ($s in $sids) {
  Write-Host "`n== $s ==" -ForegroundColor Cyan
  $remoteTar = "/tmp/off_$s.tar"
  ssh $RemoteHost "cd $RemoteRoot && tar -cf $remoteTar $exclArg $s"
  $localTar = Join-Path $Dest "$s.tar"
  scp "${RemoteHost}:$remoteTar" $localTar
  ssh $RemoteHost "rm -f $remoteTar"
  tar -xf $localTar -C $Dest
  Remove-Item $localTar
  Write-Host "  pulled -> $(Join-Path $Dest $s)"

  if ($PruneNativeAfter -and $Tier -eq "full") {
    # Deleting the Jetson's only full-quality copy demands a REAL integrity check,
    # not "the index file exists". Run the same CRC verify gate offload_pull.sh uses.
    $verify = Join-Path $PSScriptRoot "airec_dump.py"
    $sess   = Join-Path $Dest $s
    $ok = $false
    if ((Test-Path $verify) -and (Get-Command python -ErrorAction SilentlyContinue)) {
      & python $verify $sess --verify --chan eo_y10 *> $null
      $ok = ($LASTEXITCODE -eq 0)
    } else {
      Write-Host "  cannot run airec_dump.py --verify (need python) — NOT pruning" -ForegroundColor Yellow
    }
    if ($ok) {
      Write-Host "  local native VERIFIED (crc) — freeing raw on the Jetson"
      ssh $RemoteHost "curl -s 'http://127.0.0.1:$RecorderPort/ctl?purge_native=$s' >/dev/null"
    } else {
      Write-Host "  local native failed verify — NOT pruning (safety)" -ForegroundColor Yellow
    }
  }
}
Write-Host "`ndone -> $Dest" -ForegroundColor Green
