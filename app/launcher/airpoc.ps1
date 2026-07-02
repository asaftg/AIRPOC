<#
  AIRPOC laptop launcher — start/stop the Jetson-side service and open the operator
  console in the default browser. The app runs on the Jetson; this is the desktop
  button on the operator's laptop.

  Usage:  airpoc.ps1 [-Action start|stop|open] [-Target 192.168.55.1] [-User asaftg] [-Port 8080]
#>
param(
  [ValidateSet("start","stop","open")] [string]$Action = "start",
  [string]$Target = "192.168.55.1",
  [string]$User   = "asaftg",
  [int]$Port      = 8080
)

if ($Action -ne "open") {
  ssh "$User@$Target" "sudo systemctl $Action airpoc-app"
}
if ($Action -eq "start" -or $Action -eq "open") {
  Start-Process "http://${Target}:${Port}/"
}
