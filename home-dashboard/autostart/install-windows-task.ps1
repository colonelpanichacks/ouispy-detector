# Registers a Windows scheduled task that starts OUI-SPY Mesh at logon and
# restarts it if it stops. Run once from PowerShell inside home-dashboard\autostart.
$dash   = Split-Path -Parent $PSScriptRoot
$python = Join-Path $dash ".venv\Scripts\pythonw.exe"
if (-not (Test-Path $python)) { $python = Join-Path $dash ".venv\Scripts\python.exe" }
$action   = New-ScheduledTaskAction -Execute $python -Argument "ouispymesh.py" -WorkingDirectory $dash
$trigger  = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
            -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName "OUISPYMesh" -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName "OUISPYMesh"
Write-Host "OUI-SPY Mesh task registered and started: http://127.0.0.1:5077"
