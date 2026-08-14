param(
  [string]$OutputDirectory,
  [string]$DependencyBuildDirectory,
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$templateRoot = Join-Path $repoRoot 'packaging\vs2022'
if (-not $OutputDirectory) {
  $OutputDirectory = Join-Path $repoRoot 'dist\lw.Web2App-vs2022-source'
}
$target = [IO.Path]::GetFullPath($OutputDirectory)
$distRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'dist'))
if (-not $target.StartsWith($distRoot + [IO.Path]::DirectorySeparatorChar,
                            [StringComparison]::OrdinalIgnoreCase)) {
  throw "For safety, OutputDirectory must be a child of $distRoot"
}
if (Test-Path -LiteralPath $target) {
  if (-not $Force) {
    throw "Output directory already exists. Re-run with -Force to replace it: $target"
  }
  Remove-Item -LiteralPath $target -Recurse -Force
}

$candidates = @()
if ($DependencyBuildDirectory) {
  $candidates += [IO.Path]::GetFullPath($DependencyBuildDirectory)
}
$candidates += @(
  (Join-Path $repoRoot 'build-local-windows-ninja\_deps'),
  (Join-Path $repoRoot 'build-ninja\_deps'),
  (Join-Path $repoRoot 'build\_deps')
)
$depsRoot = $candidates | Where-Object {
  Test-Path -LiteralPath (Join-Path $_ 'nlohmann_json-src\include')
} | Select-Object -First 1
if (-not $depsRoot) {
  throw 'Configured dependency sources were not found. Configure the CMake project once before creating the VS2022 package.'
}

function Copy-Tree([string]$source, [string]$destination) {
  if (-not (Test-Path -LiteralPath $source)) {
    throw "Required source is missing: $source"
  }
  New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
  Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
}

New-Item -ItemType Directory -Path $target -Force | Out-Null
foreach ($directory in @('include', 'src', 'resources', 'tests', 'docs', 'examples')) {
  Copy-Tree (Join-Path $repoRoot $directory) (Join-Path $target $directory)
}
foreach ($file in @('README.md', 'README_EN.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md')) {
  Copy-Item -LiteralPath (Join-Path $repoRoot $file) -Destination $target -Force
}
Copy-Item -Path (Join-Path $templateRoot '*') -Destination $target -Force

$depsTarget = Join-Path $target 'deps'
Copy-Tree (Join-Path $depsRoot 'nlohmann_json-src\include') (Join-Path $depsTarget 'nlohmann\include')
New-Item -ItemType Directory -Path (Join-Path $depsTarget 'nlohmann') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $depsRoot 'nlohmann_json-src\LICENSE.MIT') -Destination (Join-Path $depsTarget 'nlohmann\LICENSE.MIT') -Force

New-Item -ItemType Directory -Path (Join-Path $depsTarget 'cpp-httplib') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $depsRoot 'cpp_httplib-src\httplib.h') -Destination (Join-Path $depsTarget 'cpp-httplib\httplib.h') -Force
Copy-Item -LiteralPath (Join-Path $depsRoot 'cpp_httplib-src\LICENSE') -Destination (Join-Path $depsTarget 'cpp-httplib\LICENSE') -Force

New-Item -ItemType Directory -Path (Join-Path $depsTarget 'miniz') -Force | Out-Null
Copy-Item -Path (Join-Path $depsRoot 'miniz-src\*.c') -Destination (Join-Path $depsTarget 'miniz') -Force
Copy-Item -Path (Join-Path $depsRoot 'miniz-src\*.h') -Destination (Join-Path $depsTarget 'miniz') -Force
Copy-Item -LiteralPath (Join-Path $depsRoot 'miniz-build\miniz_export.h') -Destination (Join-Path $depsTarget 'miniz\miniz_export.h') -Force
Copy-Item -LiteralPath (Join-Path $depsRoot 'miniz-src\LICENSE') -Destination (Join-Path $depsTarget 'miniz\LICENSE') -Force

Copy-Tree (Join-Path $depsRoot 'spdlog-src\include') (Join-Path $depsTarget 'spdlog\include')
Copy-Tree (Join-Path $depsRoot 'spdlog-src\src') (Join-Path $depsTarget 'spdlog\src')
Copy-Item -LiteralPath (Join-Path $depsRoot 'spdlog-src\LICENSE') -Destination (Join-Path $depsTarget 'spdlog\LICENSE') -Force

Copy-Tree (Join-Path $depsRoot 'webview2_sdk-src\build\native\include') (Join-Path $depsTarget 'webview2\build\native\include')
New-Item -ItemType Directory -Path (Join-Path $depsTarget 'webview2\build\native\x64') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $depsRoot 'webview2_sdk-src\build\native\x64\WebView2LoaderStatic.lib') -Destination (Join-Path $depsTarget 'webview2\build\native\x64\WebView2LoaderStatic.lib') -Force
Copy-Item -LiteralPath (Join-Path $depsRoot 'webview2_sdk-src\LICENSE.txt') -Destination (Join-Path $depsTarget 'webview2\LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $depsRoot 'webview2_sdk-src\NOTICE.txt') -Destination (Join-Path $depsTarget 'webview2\NOTICE.txt') -Force

$zip = "$target.zip"
if (Test-Path -LiteralPath $zip) {
  Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -Path (Join-Path $target '*') -DestinationPath $zip -CompressionLevel Optimal
$checksum = Join-Path (Split-Path $target -Parent) 'SHA256SUMS-vs2022-source.txt'
$hash = Get-FileHash -LiteralPath $zip -Algorithm SHA256
($hash.Hash.ToLowerInvariant() + '  ' + (Split-Path $zip -Leaf)) |
  Set-Content -LiteralPath $checksum -Encoding ascii

$files = Get-ChildItem -LiteralPath $target -Recurse -File
[pscustomobject]@{
  PackageDirectory = $target
  ZipFile = $zip
  ChecksumFile = $checksum
  DependencySource = $depsRoot
  FileCount = $files.Count
  UncompressedBytes = ($files | Measure-Object Length -Sum).Sum
  ZipBytes = (Get-Item -LiteralPath $zip).Length
}
