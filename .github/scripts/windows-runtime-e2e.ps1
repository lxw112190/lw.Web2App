param(
  [Parameter(Mandatory = $true)]
  [string]$Packer,

  [Parameter(Mandatory = $true)]
  [string]$Source,

  [Parameter(Mandatory = $true)]
  [string]$WorkDirectory
)

$ErrorActionPreference = 'Stop'
$appId = 'com.lwweb.ci.runtime-e2e'
$work = [System.IO.Path]::GetFullPath($WorkDirectory)
$output = Join-Path $work 'runtime-e2e.exe'
$localAppData = [Environment]::GetFolderPath('LocalApplicationData')
$runtimeRoot = Join-Path $localAppData "lw.Web2App\apps\$appId"
$logDirectory = Join-Path $runtimeRoot 'logs'
$runtimeLog = Join-Path $logDirectory 'app.log'
$collectedLog = Join-Path $work 'runtime.log'

New-Item -ItemType Directory -Force -Path $work | Out-Null
if (Test-Path -LiteralPath $logDirectory) {
  Remove-Item -LiteralPath $logDirectory -Recurse -Force
}

$pack = Start-Process -FilePath $Packer -Wait -PassThru -NoNewWindow `
  -ArgumentList @(
    'pack', [System.IO.Path]::GetFullPath($Source), $output,
    '--title', 'lw.Web2App Runtime E2E',
    '--app-id', $appId,
    '--width', '800', '--height', '520',
    '--windowed', '--ipc', '--ipc-capability', 'app.info'
  )
if ($pack.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $output)) {
  throw "Runtime E2E packaging failed with exit code $($pack.ExitCode)"
}

$process = $null
try {
  $process = Start-Process -FilePath $output -PassThru
  $deadline = [DateTime]::UtcNow.AddSeconds(60)
  $passed = $false

  while ([DateTime]::UtcNow -lt $deadline) {
    $process.Refresh()
    if ($process.HasExited) {
      throw "Generated Runtime exited before completing E2E (exit code $($process.ExitCode))"
    }

    if (Test-Path -LiteralPath $runtimeLog) {
      try {
        $text = Get-Content -LiteralPath $runtimeLog -Raw -ErrorAction Stop
        if ($text.Contains('LWWEB_E2E_IPC_FAIL')) {
          throw 'The Runtime test page reported an IPC failure'
        }
        $markers = @(
          'WebView2 initialized',
          'Navigation completed',
          'IPC request: app.getInfo',
          'IPC result: app.getInfo OK',
          "LWWEB_E2E_IPC_OK appId=$appId;platform=windows;arch=x64"
        )
        if (($markers | Where-Object { -not $text.Contains($_) }).Count -eq 0) {
          $passed = $true
          break
        }
      } catch [System.IO.IOException] {
        # spdlog may briefly hold the file while flushing the latest record.
      }
    }
    Start-Sleep -Milliseconds 500
  }

  if (-not $passed) {
    throw 'Timed out waiting for the generated Runtime and Native IPC success markers'
  }
} finally {
  if ($process) {
    $process.Refresh()
    if (-not $process.HasExited) {
      $null = $process.CloseMainWindow()
      for ($attempt = 0; $attempt -lt 10; $attempt++) {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited) { break }
      }
      if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
      }
    }
  }
  if (Test-Path -LiteralPath $runtimeLog) {
    Copy-Item -LiteralPath $runtimeLog -Destination $collectedLog -Force
  }
}

Write-Host "Windows Runtime E2E passed. Log: $collectedLog"
