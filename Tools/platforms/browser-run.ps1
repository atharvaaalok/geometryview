# Windows: opens a check page in any browser and waits for the report the page posts itself, for
# browsers headless.mjs cannot drive (Firefox) or to check one as a person would see it.
#
#   powershell -File Tools\platforms\browser-run.ps1 <browser.exe> <url path> <report file> [seconds] [screenshot.png]
#
# Starts the census and viewer servers from this repository (Node on the PATH), opens
# http://127.0.0.1<url path> in a fresh profile, waits for <report file> (relative to the
# repository) and prints it, then ends the browser and the servers. Run it on the signed-in desktop:
# a browser in the service session, as over SSH, presents nothing. Close other windows first,
# since Firefox all but stops drawing in a covered window.
param($Exe, $Path, $Report, $Seconds = 240, $Shot = "")
Set-Location (Join-Path $PSScriptRoot "..\..")
Remove-Item $Report -ErrorAction SilentlyContinue
$census = Start-Process node -ArgumentList "Web/census/serve.mjs", "7341" -NoNewWindow -PassThru
$viewer = Start-Process node -ArgumentList "Web/spike/serve.mjs", "7342" -NoNewWindow -PassThru
Start-Sleep -Seconds 1
$profile = Join-Path $env:TEMP ("geometry-browser-" + [guid]::NewGuid())
New-Item -ItemType Directory $profile | Out-Null
if ($Exe -like "*firefox*") {
  Set-Content "$profile\user.js" @'
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.aboutwelcome.enabled", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
'@
  $arguments = @("-no-remote", "-profile", $profile, "-width", "1600", "-height", "1000", "http://127.0.0.1$Path")
} else {
  $arguments = @("--user-data-dir=$profile", "--no-first-run", "--window-size=1600,1000", "http://127.0.0.1$Path")
}
$browser = Start-Process $Exe -ArgumentList $arguments -PassThru
$deadline = (Get-Date).AddSeconds($Seconds)
while (-not (Test-Path $Report) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
Start-Sleep -Seconds 2
if ($Shot) {
  # The desktop as the person would see it, browser window and all.
  Add-Type -AssemblyName System.Windows.Forms, System.Drawing
  $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
  [System.Drawing.Graphics]::FromImage($bitmap).CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
  $bitmap.Save($Shot)
}
if (Test-Path $Report) { Get-Content $Report -Raw } else { "no report: $Report" }
Get-Process | Where-Object { $_.Path -eq $Exe } | Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Id $census.Id, $viewer.Id -Force -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $profile -ErrorAction SilentlyContinue
