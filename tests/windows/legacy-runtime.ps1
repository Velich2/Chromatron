param(
    [Parameter(Mandatory = $true)][string]$TestExecutable,
    [string]$OutputDirectory = 'build/legacy-runtime'
)

$ErrorActionPreference = 'Stop'
$testSource = (Resolve-Path -LiteralPath $TestExecutable).Path
$testDirectory = (New-Item -ItemType Directory -Path $OutputDirectory -Force).FullName

# Nuke 15.1v4 preloads these MSVC 14.36.32532 libraries. Fetch the original Microsoft
# files into the test process directory, with fixed hashes, leaving the system runtime intact.
$runtimeFiles = @(
    @{
        Name = 'MSVCP140.dll'
        Key = '04A30CF08E000'
        Hash = '74892D9B4028C05DEBAF0B9B5D9DC6D22F7956FA7D7EEE00C681318C26792823'
    },
    @{
        Name = 'VCRUNTIME140.dll'
        Key = 'C94BF7881B000'
        Hash = '69320F278D90EFAAEB67E2A1B55E5B0543883125834C812C8D9C39676E0494FC'
    },
    @{
        Name = 'VCRUNTIME140_1.dll'
        Key = 'D234ED7DC000'
        Hash = '6FD366A691ED68430BCD0A3DE3D8D19A0CB2102952BFC140BBEF4354ED082C47'
    }
)

foreach ($runtimeFile in $runtimeFiles) {
    $target = Join-Path $testDirectory $runtimeFile.Name
    $url = 'https://msdl.microsoft.com/download/symbols/' + $runtimeFile.Name + '/' +
           $runtimeFile.Key + '/' + $runtimeFile.Name
    Invoke-WebRequest -Uri $url -OutFile $target
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $runtimeFile.Hash) {
        throw ('Unexpected Microsoft runtime file ' + $runtimeFile.Name)
    }
}

$legacyTest = Join-Path $testDirectory 'chromatron-legacy-tests.exe'
Copy-Item -LiteralPath $testSource -Destination $legacyTest -Force
& $legacyTest
if ($LASTEXITCODE -ne 0) {
    throw ('Tests with the Nuke 15.1 MSVC runtime failed, process exit ' + $LASTEXITCODE)
}
