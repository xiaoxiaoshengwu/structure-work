# Compile with an existing MSVC installation. Intermediate files stay in a temporary directory.
param([ValidateSet('Core', 'UI', 'Standalone')][string]$Target = 'Core')
$ErrorActionPreference = 'Stop'
$trussExitCode = 0
$trussBuildDir = $null
$trussPushed = $false
try {
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $trussVSWhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $trussVSWhere)) { throw 'MSVC not found. The supplied executables run without a compiler.' }
        $trussVS = & $trussVSWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $trussVS) { throw 'An existing MSVC x64 compiler is required to rebuild the source.' }
        $trussVCVars = Join-Path $trussVS 'VC\Auxiliary\Build\vcvars64.bat'
        $trussEnvironment = & $env:ComSpec /d /s /c "call `"$trussVCVars`" >nul && set"
        if ($LASTEXITCODE -ne 0) { throw 'MSVC environment initialization failed.' }
        foreach ($trussLine in $trussEnvironment) {
            if ($trussLine -cmatch '^(PATH|INCLUDE|LIB|LIBPATH)=(.*)$') {
                [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
                if ($Matches[1] -eq 'PATH') {
                    [Environment]::SetEnvironmentVariable('Path', $Matches[2], 'Process')
                }
            }
        }
    }
    $trussCompiler = (Get-Command cl.exe -ErrorAction Stop).Source
    $trussTempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $trussBuildDir = Join-Path $trussTempRoot ('truss-build-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $trussBuildDir | Out-Null
    Push-Location -LiteralPath $trussBuildDir
    $trussPushed = $true
    $trussSourceDir = Join-Path $PSScriptRoot 'src'
    $trussSubmitDir = Split-Path -Parent $PSScriptRoot
    $trussCommon = @('/nologo', '/std:c11', '/O2', '/W4', '/WX', '/utf-8', '/MT')
    $trussArguments = $trussCommon
    if ($Target -eq 'Core') {
        $trussOutput = 'truss.exe'
        $trussDestination = Join-Path $PSScriptRoot $trussOutput
        $trussArguments += @('/Fe:truss.exe', (Join-Path $trussSourceDir 'truss.c'))
    } else {
        $trussOutput = 'truss_ui.exe'
        $trussDestination = Join-Path $PSScriptRoot $trussOutput
        if ($Target -eq 'Standalone') {
            & $trussCompiler @trussCommon /Fe:bundle_core.exe (Join-Path $trussSourceDir 'truss.c')
            if ($LASTEXITCODE -ne 0) { throw 'Embedded solver build failed.' }
            $trussResources = @(
                @{ Id = 501; Path = (Join-Path $trussBuildDir 'bundle_core.exe') },
                @{ Id = 502; Path = (Join-Path $trussSubmitDir '总控信息\课件四杆_验证算例.txt') },
                @{ Id = 503; Path = (Join-Path $trussSubmitDir '总控信息\七杆桁架_验证算例.txt') }
            )
            $trussResourceText = "#pragma code_page(65001)`r`n" + (($trussResources | ForEach-Object {
                '{0} RCDATA "{1}"' -f $_.Id, $_.Path.Replace('\', '\\')
            }) -join "`r`n") + "`r`n"
            [IO.File]::WriteAllText((Join-Path $trussBuildDir 'bundle.rc'), $trussResourceText, (New-Object Text.UTF8Encoding($true)))
            & rc.exe /nologo /fo bundle.res bundle.rc
            if ($LASTEXITCODE -ne 0) { throw 'Embedded resources build failed.' }
            $trussOutput = 'truss_standalone.exe'
            $trussDestination = Join-Path $trussSubmitDir '平面桁架计算.exe'
            $trussArguments += '/DTRUSS_STANDALONE'
        }
        $trussArguments += @("/Fe:$trussOutput", (Join-Path $trussSourceDir 'truss_ui.c'),
            (Join-Path $trussSourceDir 'truss_ui_io.c'), (Join-Path $trussSourceDir 'truss_ui_view.c'))
        if ($Target -eq 'Standalone') { $trussArguments += 'bundle.res' }
        $trussArguments += @('/link', '/SUBSYSTEM:WINDOWS', '/MANIFEST:EMBED',
            'user32.lib', 'gdi32.lib', 'comctl32.lib', 'comdlg32.lib', 'shell32.lib')
    }
    & $trussCompiler @trussArguments
    if ($LASTEXITCODE -ne 0) { throw 'Build failed. Read the compiler messages above.' }
    Copy-Item -LiteralPath (Join-Path $trussBuildDir $trussOutput) -Destination $trussDestination -Force
    Write-Host "Build succeeded: $trussDestination"
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    $trussExitCode = 1
} finally {
    if ($trussPushed) { Pop-Location }
    if ($trussBuildDir -and (Test-Path -LiteralPath $trussBuildDir)) {
        $trussResolved = [IO.Path]::GetFullPath($trussBuildDir)
        $trussItem = Get-Item -LiteralPath $trussResolved
        if ($trussItem.Parent.FullName.TrimEnd('\') -ne $trussTempRoot -or
            $trussItem.Name -notlike 'truss-build-*' -or
            ($trussItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Temporary build directory cleanup refused: unexpected path.'
        }
        Remove-Item -LiteralPath $trussResolved -Recurse -Force
    }
}
exit $trussExitCode
