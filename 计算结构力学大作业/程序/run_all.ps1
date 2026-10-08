# Windows launch helper; all numerical work is performed by truss.exe.
$ErrorActionPreference = 'Stop'
$trussRoot = Split-Path -Parent $PSScriptRoot
$trussInputDir = Join-Path $trussRoot '总控信息'
$trussOutputDir = Join-Path $trussRoot '计算结果'
$trussExe = Join-Path $PSScriptRoot 'truss.exe'
$trussCulture = [System.Globalization.CultureInfo]::InvariantCulture
function Read-TrussNumber([string]$message) {
    while ($true) {
        $trussText = Read-Host $message
        [double]$trussValue = 0
        $trussValid = [double]::TryParse($trussText,
            [System.Globalization.NumberStyles]::Float, $trussCulture, [ref]$trussValue)
        if ($trussValid -and $trussValue -gt 0 -and
            -not [double]::IsNaN($trussValue) -and -not [double]::IsInfinity($trussValue)) {
            return $trussValue
        }
        Write-Host 'Please enter a finite positive number, using a decimal point.'
    }
}
try {
    Write-Host 'Required problem: base length l and top height h are parameters.'
    Write-Host 'The diagram gives no numerical dimensions. Enter both parameters explicitly.'
    Write-Host 'For a numerical verification only, you may use l=1 m and h=1 m.'
    $trussLength = Read-TrussNumber 'Base member 6 length l (m)'
    do {
        $trussHeight = Read-TrussNumber 'Node 4 height h (m)'
        $trussShapeValid = $trussHeight -gt ($trussLength / (2 * [Math]::Sqrt(3)))
        if (-not $trussShapeValid) { Write-Host 'h must exceed l/(2*sqrt(3)).' }
    } while (-not $trussShapeValid)
    New-Item -ItemType Directory -Force -Path $trussInputDir,$trussOutputDir | Out-Null
    $trussL = $trussLength.ToString('G17',$trussCulture)
    $trussH = $trussHeight.ToString('G17',$trussCulture)
    $trussRequiredInput = Join-Path $trussInputDir '必做题_总控信息.txt'
    $trussRequiredOutput = Join-Path $trussOutputDir '必做题_机算结果.txt'
    & $trussExe --write-required $trussL $trussH $trussRequiredInput
    if ($LASTEXITCODE -ne 0) { throw 'Required input generation failed.' }
    & $trussExe --required $trussL $trussH $trussRequiredOutput
    if ($LASTEXITCODE -ne 0) { throw 'Required problem calculation failed.' }
    Write-Host "Required result: $trussRequiredOutput"
    $trussOptionalInput = Join-Path $trussInputDir '七杆桁架_验证算例.txt'
    if (Test-Path -LiteralPath $trussOptionalInput) {
        $trussOptionalOutput = Join-Path $trussOutputDir '七杆桁架_验证结果.txt'
        & $trussExe $trussOptionalInput $trussOptionalOutput
        if ($LASTEXITCODE -ne 0) { throw 'Seven-member verification calculation failed.' }
        Write-Host "Seven-member verification result: $trussOptionalOutput"
    } else { Write-Host "Seven-member verification input not found: $trussOptionalInput" }
    exit 0
} catch { Write-Host $_.Exception.Message; exit 1 }
