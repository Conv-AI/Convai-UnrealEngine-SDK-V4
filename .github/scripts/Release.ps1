param([ValidateSet('Functions','Plan','Check','Preflight','Notes','Tag')][string]$Mode = 'Functions')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-ReleaseGit([string[]]$Arguments) {
    $result = @(& git @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Git failed: $($Arguments[0])" }
    return ,$result
}

function Get-ReleaseTags([string[]]$Lines) {
    $tags = @{}
    foreach ($line in $Lines) {
        if ($line -match '^([0-9a-f]{40})\s+refs/tags/(.+?)(\^\{\})?$') {
            # Peeled annotated tags identify the commit, not the tag object.
            if (-not $tags.ContainsKey($Matches[2]) -or $Matches[3]) { $tags[$Matches[2]] = $Matches[1] }
        }
    }
    return $tags
}

function New-ReleasePlan {
    param([string]$BaseVersion, [string]$Branch, [string]$Sha, [string]$Event,
        [hashtable]$Inputs, [hashtable]$Tags, [int]$RunAttempt = 1)
    if ($BaseVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$') { throw 'Invalid base VersionName.' }
    if ($Sha -notmatch '^[0-9a-f]{40}$') { throw 'Invalid source commit.' }
    if ([string]::IsNullOrWhiteSpace($Branch) -or $Branch.Length -gt 200 -or $Branch -match '[\x00-\x20\x7f]') { throw 'Invalid source branch.' }
    $normal = $Branch -cin @('main','staging','beta')
    if ($Event -notin @('push','workflow_dispatch') -or ($Event -eq 'push' -and -not $normal)) { throw 'Unsupported release event or push branch.' }
    $versions = @('5.3','5.4','5.5','5.6','5.7','5.8')
    if ($Event -eq 'workflow_dispatch') {
        $versions = @($versions | Where-Object {
            $key = 'ue_' + $_.Replace('.','_')
            if ($Inputs.ContainsKey($key)) {
                $value = [string]$Inputs[$key]
                if ($value -notmatch '^(true|false)$') { throw "Invalid engine checkbox: $key" }
                $value -eq 'true'
            } else { $_ -in @('5.7','5.8') }
        })
    }
    if ($versions.Count -eq 0) { throw 'Select at least one Unreal Engine version.' }
    $platforms = @('Win64','Android')
    if ($Event -eq 'workflow_dispatch') {
        $platforms = @()
        foreach ($choice in @(@('windows','Win64','true'), @('android','Android','false'))) {
            $value = if ($Inputs.ContainsKey($choice[0])) { [string]$Inputs[$choice[0]] } else { $choice[2] }
            if ($value -notmatch '^(true|false)$') { throw "Invalid platform checkbox: $($choice[0])" }
            if ($value -eq 'true') { $platforms += $choice[1] }
        }
    }
    if ($platforms.Count -eq 0) { throw 'Select Windows, Android, or both platforms.' }
    $requested = if ($Inputs.ContainsKey('build_number')) { [string]$Inputs.build_number } else { '' }
    if ($requested -and $requested -notmatch '^[1-9][0-9]{0,8}$') { throw 'Build number must be empty or a positive integer below 1000000000.' }
    if ($normal -and $requested) { throw 'Build number applies only to feature branches.' }
    $version = $BaseVersion
    if (-not $normal) {
        # Preserve familiar feat/name tags; other branch namespaces remain distinct.
        $slug = if ($Branch -cmatch '^feat/[a-z0-9][a-z0-9-]{0,59}$') { $Branch.Substring(5) } else {
            $hash = [Security.Cryptography.SHA256]::Create()
            try { $suffix = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Branch))).Replace('-','').Substring(0,8).ToLowerInvariant() } finally { $hash.Dispose() }
            'branch-' + ($Branch.ToLowerInvariant() -replace '[^a-z0-9]+','-').Trim('-').Substring(0,[Math]::Min(40,($Branch.ToLowerInvariant() -replace '[^a-z0-9]+','-').Trim('-').Length)) + '-' + $suffix
        }
        $prefix = "$BaseVersion-$slug."
        $existing = @($Tags.Keys | Where-Object { $_ -cmatch ('^' + [regex]::Escape($prefix) + '[1-9][0-9]{0,8}$') })
        $sameCommit = @($existing | Where-Object { $Tags[$_] -eq $Sha } | Sort-Object { [int]$_.Substring($prefix.Length) } -Descending)
        if ($requested) { $number = [int]$requested }
        elseif ($RunAttempt -gt 1 -and $sameCommit.Count) { $number = [int]$sameCommit[0].Substring($prefix.Length) }
        else {
            $maximum = 0
            foreach ($tag in $existing) { $maximum = [Math]::Max($maximum,[int]$tag.Substring($prefix.Length)) }
            $number = $maximum + 1
            if ($number -ge 1000000000) { throw 'Feature build number range exhausted.' }
        }
        $version = $prefix + $number
    }
    $expectedTags = @($version)
    if ($normal) { $expectedTags += "marketplace-$version" }
    foreach ($tag in $expectedTags) {
        if ($Tags.ContainsKey($tag) -and $Tags[$tag] -ne $Sha) { throw "Tag '$tag' already identifies a different commit. Choose another feature build number or update the normal release version." }
    }
    return [ordered]@{ schema = 2; base_version = $BaseVersion; version = $version; branch = $Branch; sha = $Sha; feature = (-not $normal); versions = @($versions); platforms = @($platforms) }
}

function Assert-ReleasePlan($Plan) {
    if ($null -eq $Plan -or ($Plan.schema -isnot [int] -and $Plan.schema -isnot [long]) -or $Plan.schema -ne 2 -or
        $Plan.version -isnot [string] -or $Plan.version -notmatch '^[0-9A-Za-z.-]+$' -or
        $Plan.base_version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$' -or
        $Plan.sha -cnotmatch '^[0-9a-f]{40}$' -or $Plan.feature -isnot [bool] -or
        [string]::IsNullOrWhiteSpace($Plan.branch) -or $Plan.branch -match '[\x00-\x20\x7f]' -or $Plan.branch.Length -gt 200) {
        throw 'Invalid release plan. A schema-2 plan with explicit engine and platform coverage is required.'
    }
    foreach ($field in @('versions','platforms')) {
        $values = $Plan.$field
        if ($values -is [string] -or $values -isnot [Collections.IEnumerable] -or @($values).Count -eq 0) { throw "Invalid planned $field selection." }
        $allowed = if ($field -eq 'versions') { @('5.3','5.4','5.5','5.6','5.7','5.8') } else { @('Win64','Android') }
        $seen = @{}
        foreach ($value in $values) {
            if ($value -isnot [string] -or $value -cnotin $allowed -or $seen.ContainsKey($value)) { throw "Invalid or duplicate planned $field selection." }
            $seen[$value] = $true
        }
    }
}

function Get-ReleaseArchiveName {
    param([string]$Engine, [string[]]$Platforms, [bool]$Marketplace = $false)
    if ($Engine -cnotin @('5.3','5.4','5.5','5.6','5.7','5.8')) { throw 'Invalid archive engine.' }
    if ($Platforms.Count -lt 1 -or $Platforms.Count -gt 2 -or @($Platforms | Select-Object -Unique).Count -ne $Platforms.Count) { throw 'Invalid archive platform selection.' }
    foreach ($platform in $Platforms) { if ($platform -cnotin @('Win64','Android')) { throw 'Invalid archive platform.' } }
    $suffix = if ($Platforms.Count -eq 1) { '-' + $Platforms[0] } else { '' }
    if ($Marketplace) { $suffix += '-marketplace-no-binaries' }
    return "Convai-UE$Engine$suffix.zip"
}

function Get-ReleaseArchiveNames {
    param($Plan, [bool]$Marketplace = $false)
    Assert-ReleasePlan $Plan
    return @($Plan.versions | ForEach-Object { Get-ReleaseArchiveName $_ $Plan.platforms $Marketplace })
}

function Assert-ReleaseCoverage($Expected, $Published) {
    Assert-ReleasePlan $Expected
    if ($null -eq $Published) { throw 'The existing tag/release has no release-plan.json coverage manifest. Choose a new feature build number or release version; existing ZIP names cannot establish platform coverage.' }
    Assert-ReleasePlan $Published
    foreach ($field in @('base_version','version','branch','sha','feature')) {
        if ($Expected.$field -cne $Published.$field) { throw "Existing release $field differs. Choose a new feature build number or release version; existing assets will not be overwritten." }
    }
    foreach ($field in @('versions','platforms')) {
        if ((@($Expected.$field | Sort-Object) -join ',') -cne (@($Published.$field | Sort-Object) -join ',')) {
            throw "Existing release $field coverage differs. Choose a new feature build number or release version; existing assets will not be overwritten."
        }
    }
}

function Test-ReleaseAssetCoverage {
    param($Plan, $Release, $PublishedPlan, [bool]$Marketplace = $false)
    Assert-ReleaseCoverage $Plan $PublishedPlan
    if ($Release.draft -isnot [bool]) { throw 'Invalid published release state.' }
    $expected = @(Get-ReleaseArchiveNames $Plan $Marketplace)
    $names = @($Release.assets | ForEach-Object { [string]$_.name })
    if (@($names | Select-Object -Unique).Count -ne $names.Count) { throw 'Existing release contains duplicate asset names.' }
    foreach ($name in $names) {
        if ($name -match '^Convai-UE.*\.zip$' -and $name -cnotin $expected) { throw 'Existing release ZIP names contradict its coverage manifest. Choose a new build number or release version.' }
    }
    if ($Release.draft) { return $false }
    foreach ($name in $expected) {
        $asset = @($Release.assets | Where-Object { $_.name -ceq $name })
        if ($asset.Count -ne 1 -or ($asset[0].size -isnot [int] -and $asset[0].size -isnot [long]) -or $asset[0].size -le 0) { return $false }
    }
    return $true
}

function Assert-UnpublishedReleaseTag($Plan, [string]$Tag, [hashtable]$Tags) {
    # A tag alone has no published artifacts to replace. Recover an outage after
    # tagging only when the existing tag still identifies this exact source.
    if ($Tags.ContainsKey($Tag) -and $Tags[$Tag] -cne $Plan.sha) {
        throw 'The unpublished release tag points to another source commit. Choose a new build number or release version.'
    }
}

function Get-PublishedReleaseState {
    param($Plan, [string]$Tag, [hashtable]$Tags, [bool]$Marketplace = $false)
    if ($env:GITHUB_REPOSITORY -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Invalid GitHub repository identity.' }
    $headers = @{ Authorization="Bearer $env:GH_TOKEN"; Accept='application/vnd.github+json'; 'X-GitHub-Api-Version'='2022-11-28' }
    try { $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$env:GITHUB_REPOSITORY/releases/tags/$([Uri]::EscapeDataString($Tag))" -Headers $headers -TimeoutSec 30 }
    catch {
        $responseProperty = $_.Exception.PSObject.Properties['Response']
        if ($responseProperty -and $responseProperty.Value -and [int]$responseProperty.Value.StatusCode -eq 404) {
            Assert-UnpublishedReleaseTag $Plan $Tag $Tags
            return $false
        }
        throw
    }
    if (-not $Tags.ContainsKey($Tag) -or $Tags[$Tag] -cne $Plan.sha -or $release.tag_name -cne $Tag) { throw 'Existing release tag does not match the exact planned source commit.' }
    $manifests = @($release.assets | Where-Object { $_.name -ceq 'release-plan.json' })
    if ($manifests.Count -ne 1) { Assert-ReleaseCoverage $Plan $null }
    $asset = $manifests[0]
    if ([string]$asset.id -notmatch '^[1-9][0-9]*$' -or ($asset.size -isnot [int] -and $asset.size -isnot [long]) -or $asset.size -le 0 -or $asset.size -gt 65536) { throw 'Invalid published release coverage manifest asset.' }
    $headers.Accept = 'application/octet-stream'
    # Use the verified GitHub asset ID, never an arbitrary URL supplied by release metadata.
    $published = Invoke-RestMethod -Uri "https://api.github.com/repos/$env:GITHUB_REPOSITORY/releases/assets/$($asset.id)" -Headers $headers -TimeoutSec 30
    if ($published -is [byte[]]) { $published = [Text.Encoding]::UTF8.GetString($published) }
    if ($published -is [string]) {
        if ($published.Length -gt 65536) { throw 'Published release coverage manifest is too large.' }
        $published = $published | ConvertFrom-Json
    }
    return Test-ReleaseAssetCoverage $Plan $release $published $Marketplace
}

function Get-ReleaseNotes($Plan, [string[]]$Lines) {
    $header = if ($Plan.feature) { '^#+\s*Unreleased\s*$' } else { '^#\s*Release\s+' + [regex]::Escape($Plan.base_version) + '\s*$' }
    $start = -1
    for ($i=0; $i -lt $Lines.Count; $i++) { if ($Lines[$i] -match $header) { $start=$i; break } }
    if ($start -lt 0) { throw "CHANGELOG.md has no matching section for $($Plan.version)." }
    $body = [Collections.Generic.List[string]]::new()
    for ($i=$start+1; $i -lt $Lines.Count; $i++) { if ($Lines[$i] -match '^#\s+(Release\s+|Unreleased\s*$)') { break }; $body.Add($Lines[$i]) }
    $notes = ($body -join "`n").Trim()
    if (-not $notes) { throw 'The matching CHANGELOG.md section is empty.' }
    if ($Plan.feature) {
        return "Feature preview from ``$($Plan.branch)`` at ``$($Plan.sha)``.`n`nBase SDK: **$($Plan.base_version)**. Unreal Engine: **$($Plan.versions -join ', ')**. Game platforms: **$($Plan.platforms -join ', ')**. The Windows host editor is included. This prerelease does not replace the normal SDK release.`n`n$notes"
    }
    return "Unreal Engine: **$($Plan.versions -join ', ')**. Game platforms: **$($Plan.platforms -join ', ')**. The Windows host editor is included.`n`n$notes"
}

function Read-ReleasePlan {
    $plan = $env:RELEASE_PLAN_JSON | ConvertFrom-Json
    Assert-ReleasePlan $plan
    return $plan
}

function Write-ReleaseOutput([string]$Name, [string]$Value) {
    if ($Value -match '[\r\n]') { throw 'Release output must fit on one line.' }
    [IO.File]::AppendAllText($env:GITHUB_OUTPUT, "$Name=$Value`n", [Text.UTF8Encoding]::new($false))
}

if ($Mode -eq 'Functions') { return }
if ($Mode -eq 'Plan') {
    if ($env:GITHUB_REF_TYPE -ne 'branch') { throw 'Dispatch this workflow from a branch, not a tag.' }
    $inputObject = $env:RELEASE_INPUTS_JSON | ConvertFrom-Json
    $inputs = @{}; foreach ($property in $inputObject.PSObject.Properties) { $inputs[$property.Name] = $property.Value }
    $descriptor = Get-Content -LiteralPath 'ConvAI.uplugin' -Raw | ConvertFrom-Json
    $tags = Get-ReleaseTags (Invoke-ReleaseGit @('ls-remote','--tags','origin'))
    $plan = New-ReleasePlan $descriptor.VersionName $env:GITHUB_REF_NAME $env:GITHUB_SHA $env:GITHUB_EVENT_NAME $inputs $tags ([int]$env:GITHUB_RUN_ATTEMPT)
    Write-ReleaseOutput 'version_name' $plan.version
    Write-ReleaseOutput 'feature' ([string]$plan.feature).ToLowerInvariant()
    Write-ReleaseOutput 'plan' ($plan | ConvertTo-Json -Depth 10 -Compress)
    Write-Host "Release $($plan.version); source $($plan.branch); UE $($plan.versions -join ', '); platforms $($plan.platforms -join ', ')"
    return
}
$plan = Read-ReleasePlan
if ($Mode -eq 'Check') {
    $tags = Get-ReleaseTags (Invoke-ReleaseGit @('ls-remote','--tags','origin'))
    $complete = Get-PublishedReleaseState $plan $plan.version $tags
    if (-not $plan.feature) {
        $marketplaceComplete = Get-PublishedReleaseState $plan "marketplace-$($plan.version)" $tags $true
        $complete = $complete -and $marketplaceComplete
    }
    Write-ReleaseOutput 'release_exists' ([string]$complete).ToLowerInvariant()
    return
}
if ($Mode -in @('Preflight','Notes')) {
    $notes = Get-ReleaseNotes $plan (Get-Content -LiteralPath 'CHANGELOG.md')
    if ($Mode -eq 'Notes') { [IO.File]::WriteAllText((Join-Path $PWD 'release_notes.md'),$notes,[Text.UTF8Encoding]::new($false)); return }
    $missing = @($plan.versions | Where-Object { -not (Test-Path -LiteralPath "E:/Software/UE_$_/Engine/Build/BatchFiles/RunUAT.bat") })
    if ($missing.Count) { throw "Install the selected Unreal Engine versions on the runner: $($missing -join ', ')." }
    Write-Host $notes
    return
}
if ($Mode -eq 'Tag') {
    $tags = Get-ReleaseTags (Invoke-ReleaseGit @('ls-remote','--tags','origin'))
    $expected = @($plan.version); if (-not $plan.feature) { $expected += "marketplace-$($plan.version)" }
    # Recheck coverage immediately before publication, even when Check ran before a long build.
    foreach ($tag in $expected) { Get-PublishedReleaseState $plan $tag $tags ($tag.StartsWith('marketplace-')) | Out-Null }
    foreach ($tag in $expected) {
        if ($tags.ContainsKey($tag)) {
            if ($tags[$tag] -ne $plan.sha) { throw "Remote tag $tag changed during this build; refusing to publish." }
        } else {
            # Explicit source SHA and ref; checkout authentication remains managed by the action.
            Invoke-ReleaseGit @('push','origin',"$($plan.sha):refs/tags/$tag") | Out-Null
        }
    }
}
