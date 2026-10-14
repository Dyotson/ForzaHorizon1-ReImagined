# Installs the Windows installer silently, checks what it should have put on the machine, runs
# the launcher's --version, uninstalls and checks that everything is gone. Used by the
# installer-test job of .github/workflows/release.yml and by hand on a Windows VM:
#   powershell -File test_installer.ps1 -Setup ForzaHorizon1-ReImagined-Setup-0.1.1.exe
param(
    [Parameter(Mandatory = $true)][string]$Setup,
    [string]$ExpectVersion = ""
)
$ErrorActionPreference = "Stop"
$Setup = (Resolve-Path $Setup).Path
$short = "Forza Horizon 1 - ReImagined"
$app = Join-Path $env:LOCALAPPDATA "Programs\ForzaHorizon1-ReImagined"
$start = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\$short.lnk"
$desktop = Join-Path ([Environment]::GetFolderPath("Desktop")) "$short.lnk"
$uninstKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{B0C6F2A4-5D1E-4D0B-9B1B-7B5A0F3E6C21}_is1"
$log = Join-Path $env:TEMP "forza-setup.log"
$failed = @()
function Check($ok, $what) {
    if ($ok) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:failed += $what }
}

$p = Start-Process $Setup -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER",
    "/TASKS=desktopicon", "/LOG=$log" -Wait -PassThru
Check ($p.ExitCode -eq 0) "installer exit code 0 (was $($p.ExitCode))"
if ($p.ExitCode -ne 0 -and (Test-Path $log)) { Get-Content $log -Tail 40 }
$exe = Join-Path $app "forza-launcher.exe"
Check (Test-Path $exe) "forza-launcher.exe in $app"
Check (Test-Path (Join-Path $app "project\forza-recomp\CMakeLists.txt")) "project sources installed"
Check (Test-Path (Join-Path $app "sdk-overlay\rexglue-overlay-windows-amd64.tar.gz")) "SDK overlay installed"
Check (Test-Path $start) "Start menu shortcut"
Check (Test-Path $desktop) "desktop shortcut"
Check (Test-Path $uninstKey) "uninstall entry in the registry"

if (Test-Path $exe) {
    $v = (& $exe --version | Out-String).Trim()
    Write-Host "forza-launcher --version -> $v"
    Check ($LASTEXITCODE -eq 0 -and $v -ne "") "--version runs"
    if ($ExpectVersion) { Check ($v -eq $ExpectVersion) "--version is $ExpectVersion" }
    $d = (& $exe --dry-run 2>&1 | Out-String)
    # exit 1 and a readable "game not found" message is the expected answer on a machine without the game
    Check (($LASTEXITCODE -le 1) -and $d.Trim() -ne "") "--dry-run runs and prints something (exit $LASTEXITCODE)"
}

$unins = Join-Path $app "unins000.exe"
Check (Test-Path $unins) "uninstaller present"
if (Test-Path $unins) {
    $u = Start-Process $unins -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART" -Wait -PassThru
    Check ($u.ExitCode -eq 0) "uninstaller exit code 0 (was $($u.ExitCode))"
    # the uninstaller hands the removal to a temporary copy; give it a moment
    for ($i = 0; $i -lt 30 -and (Test-Path $app); $i++) { Start-Sleep -Seconds 1 }
}
Check (-not (Test-Path $exe)) "forza-launcher.exe removed"
Check (-not (Test-Path $start)) "Start menu shortcut removed"
Check (-not (Test-Path $desktop)) "desktop shortcut removed"
Check (-not (Test-Path $uninstKey)) "uninstall entry removed"

if ($failed.Count -gt 0) { Write-Host "`n$($failed.Count) check(s) failed"; exit 1 }
Write-Host "`nall checks passed"
exit 0
