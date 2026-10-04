param(
    [Parameter(Mandatory = $true)][string]$HostFile,
    [Parameter(Mandatory = $true)][string]$BackendFile,
    [Parameter(Mandatory = $true)][string]$SeedFile,
    [Parameter(Mandatory = $true)][string]$ReadObj,
    [Parameter(Mandatory = $true)][string]$RuntimeDirectories
)
$ErrorActionPreference = 'Stop'
$hostArtifact = (Get-Item -LiteralPath $HostFile).FullName
$backendArtifact = (Get-Item -LiteralPath $BackendFile).FullName
$readObjArtifact = (Get-Item -LiteralPath $ReadObj).FullName
$packageRoot = Split-Path -Parent $hostArtifact
$seed = Get-Content -Raw -LiteralPath $SeedFile | ConvertFrom-Json
if ($seed.backend_build_id -notmatch '^vulkan-[0-9a-f]+$') { throw 'Unsafe backend build ID' }
$stagedBackend = Join-Path $packageRoot ('backends\vulkan\' + $seed.backend_build_id + '\deren_vulkan.dll')
$stagedGlfw = Join-Path $packageRoot 'glfw3.dll'
foreach ($artifact in @($stagedBackend, $stagedGlfw)) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) { throw "Package artifact missing: $artifact" }
}
if ((Get-FileHash -LiteralPath $stagedGlfw -Algorithm SHA256).Hash.ToLowerInvariant() -ne $seed.glfw_sha256) {
    throw 'The staged GLFW DLL differs from the embedded package identity'
}

function Read-PeNames([string]$Artifact, [string]$Mode) {
    $output = @(& $readObjArtifact $Mode $Artifact 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "llvm-readobj failed for ${Artifact}: $($output -join [Environment]::NewLine)" }
    $names = @()
    foreach ($line in $output) {
        if ([string]$line -match '^\s*Name:\s*(\S+)\s*$') { $names += $Matches[1] }
    }
    return @($names | Sort-Object -Unique)
}
$expectedExports = @('deren_abi_version', 'deren_destroy_api_core', 'deren_make_api_core')
$actualExports = @(Read-PeNames $stagedBackend '--coff-exports')
if (@(Compare-Object $expectedExports $actualExports).Count -ne 0) {
    throw "The backend must export exactly the three contract entries; got: $($actualExports -join ', ')"
}
$hostImports = @(Read-PeNames $hostArtifact '--coff-imports')
$backendImports = @(Read-PeNames $stagedBackend '--coff-imports')
if (@($hostImports | Where-Object { $_ -ieq 'deren_vulkan.dll' }).Count) {
    throw 'The host still statically imports the concrete backend DLL'
}
foreach ($item in @(@{ Name = 'host'; Imports = $hostImports }, @{ Name = 'backend'; Imports = $backendImports })) {
    if (-not @($item.Imports | Where-Object { $_ -ieq 'glfw3.dll' }).Count) {
        throw "The $($item.Name) must import the common shared glfw3.dll"
    }
}

# 只从明确构建目录解析运行库；不把开发机 PATH 或 Vulkan 驱动当发布依赖。
$searchDirectories = @($RuntimeDirectories.Split('|') | Where-Object { $_ } | ForEach-Object {
    if (Test-Path -LiteralPath $_ -PathType Container) { (Get-Item -LiteralPath $_).FullName }
})
$searchDirectories += (Split-Path -Parent $backendArtifact)
$searchDirectories = @($searchDirectories | Select-Object -Unique)
$queue = New-Object 'System.Collections.Generic.Queue[string]'
foreach ($artifact in @($hostArtifact, $stagedBackend, $stagedGlfw)) { $queue.Enqueue($artifact) }
$visited = @{}
$resolved = @{}
$edges = New-Object 'System.Collections.Generic.List[object]'
$external = New-Object 'System.Collections.Generic.List[object]'
$artifacts = New-Object 'System.Collections.Generic.List[object]'
$systemRoot = [Environment]::GetFolderPath('Windows')
$systemDirectory = Join-Path $systemRoot 'System32'
$redistributablePattern = '^(msvcp[0-9]+|vcruntime[0-9]+|concrt[0-9]+|libc\+\+|libunwind|libstdc\+\+-[0-9]+|libgcc_s_.+|libwinpthread-[0-9]+).*\.dll$'
while ($queue.Count) {
    $artifact = $queue.Dequeue()
    if ($visited.ContainsKey($artifact)) { continue }
    $visited[$artifact] = $true
    $artifacts.Add([ordered]@{
        path = $artifact.Substring($packageRoot.Length).TrimStart('\', '/')
        sha256 = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    })
    foreach ($name in @(Read-PeNames $artifact '--coff-imports')) {
        if ($name -notmatch '^[A-Za-z0-9_.+-]+\.dll$') { throw "Unsafe PE import name: $name" }
        $edge = [ordered]@{ importer = $artifact.Substring($packageRoot.Length).TrimStart('\', '/'); name = $name }
        if ($name -ieq 'vulkan-1.dll') {
            $edge.classification = 'external_vulkan_loader_driver'
            $external.Add([ordered]@{ name = $name; reason = 'Install the target GPU vendor Vulkan driver/loader separately' })
        } elseif ($name -match '^(api-ms-win-|ext-ms-win-)') {
            $edge.classification = 'windows_api_set'
        } elseif ($name -notmatch $redistributablePattern -and
                  (Test-Path -LiteralPath (Join-Path $systemDirectory $name) -PathType Leaf)) {
            $edge.classification = 'windows_system'
        } else {
            $edge.classification = 'packaged_runtime'
            if (-not $resolved.ContainsKey($name)) {
                if ($name -ieq 'glfw3.dll') {
                    $resolved[$name] = $stagedGlfw
                } else {
                    $candidates = @($searchDirectories | ForEach-Object {
                        $candidate = Join-Path $_ $name
                        if (Test-Path -LiteralPath $candidate -PathType Leaf) { (Get-Item -LiteralPath $candidate).FullName }
                    } | Select-Object -Unique)
                    if (-not $candidates.Count) { throw "Missing runtime DLL ${name}, imported by $artifact" }
                    $digests = @($candidates | ForEach-Object {
                        (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
                    } | Select-Object -Unique)
                    if ($digests.Count -ne 1) { throw "Ambiguous runtime DLL ${name}: $($candidates -join ', ')" }
                    $destination = Join-Path $packageRoot $name
                    if ($candidates[0] -ine $destination) { Copy-Item -LiteralPath $candidates[0] -Destination $destination -Force }
                    $resolved[$name] = $destination
                }
            }
            $edge.package_path = [IO.Path]::GetFileName($resolved[$name])
            $queue.Enqueue($resolved[$name])
        }
        $edges.Add($edge)
    }
}
$report = [ordered]@{
    backend_build_id = $seed.backend_build_id
    compat_id = $seed.compat_id
    inspection_tool = $readObjArtifact
    host_backend_import_count = 0
    backend_exports = $actualExports
    artifacts = @($artifacts.ToArray())
    imports = @($edges.ToArray())
    external_requirements = @($external.ToArray())
    startup_verified = $false
    gpu_verified = $false
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $packageRoot 'runtime_dependencies.json') -Encoding utf8
# 校验完成才激活供下一次启动选择的新目录；当前进程不换后端。
$selectionPath = Join-Path $packageRoot 'backend.selection.json'
$selectionTemp = Join-Path $packageRoot ('backend.selection.' + [Guid]::NewGuid().ToString('N') + '.tmp')
$selection = [ordered]@{ selection_schema = 1; backend_build_id = $seed.backend_build_id }
[IO.File]::WriteAllText($selectionTemp, ($selection | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
Move-Item -LiteralPath $selectionTemp -Destination $selectionPath -Force
Write-Output "Packaged $($artifacts.Count) actual PE artifacts; clean-machine startup and GPU acceptance remain unverified."
