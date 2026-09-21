param(
    [Parameter(Mandatory=$true)][string]$PluginRoot,
    [Parameter(Mandatory=$true)][string]$EngineRoot,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$version = Get-Content -LiteralPath (Join-Path $EngineRoot 'Engine/Build/Build.version') -Raw | ConvertFrom-Json
$descriptorPath = Join-Path $PluginRoot 'ConvAI.uplugin'
$descriptor = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json

# Prepare a version-specific installation, never a shared source checkout.
# Start each engine's installation from the original descriptor. Newer engines
# are deliberately a byte-for-byte no-op, including 5.6 and 5.7.
$unsupported = @('ConvaiSceneTagging', 'ConvaiSceneTaggingEditor')
if ($version.MajorVersion -ne 5 -or $version.MinorVersion -ge 6) {
    if ($CheckOnly -and @($descriptor.Modules | Where-Object Name -In $unsupported).Count -ne 2) {
        throw 'Supported-engine package must retain both Scene Auto Tagger modules. Start from the original descriptor.'
    }
    Write-Host "UE $($version.MajorVersion).$($version.MinorVersion): module descriptor unchanged."
    return
}
$present = @($descriptor.Modules | Where-Object Name -In $unsupported)
if ($CheckOnly) {
    if ($present.Count) { throw 'Pre-5.6 installation still enables Scene Auto Tagger modules.' }
    Write-Host "UE5.$($version.MinorVersion) module selection PASS."
    return
}
if ($present.Count) {
    $descriptor.Modules = @($descriptor.Modules | Where-Object Name -NotIn $unsupported)
    [IO.File]::WriteAllText($descriptorPath, ($descriptor | ConvertTo-Json -Depth 50), [Text.UTF8Encoding]::new($false))
}
Write-Host "UE5.$($version.MinorVersion): Scene Auto Tagger excluded; chatbot, audio, animation and Scene Objects modules retained."
