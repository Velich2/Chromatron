param(
    [Parameter(Mandatory = $true)][string]$TestExecutable,
    [string]$OutputDirectory = 'build/legacy-runtime',
    [ValidateSet('Nuke151', 'Premiere25')][string]$RuntimeProfile = 'Nuke151',
    [string]$RuntimeDirectory
)

$ErrorActionPreference = 'Stop'
$testSource = (Resolve-Path -LiteralPath $TestExecutable).Path
$testDirectory = (New-Item -ItemType Directory -Path $OutputDirectory -Force).FullName

# Hosts preload their own MSVC libraries. Isolate the originals in the test process
# directory, with fixed hashes, leaving the system runtime intact.
$runtimeFiles = if ($RuntimeProfile -eq 'Premiere25') {
    # Premiere Pro 25.0.0.61: MSVC 14.23.27820.
    # Use the host's local originals for this profile.
    if (!$RuntimeDirectory) {
        throw 'Premiere25 requires -RuntimeDirectory pointing to a Premiere Pro 25.0.0.61 installation'
    }
    @(
        @{
            Name = 'MSVCP140.dll'
            Hash = 'A2199BAD3D7BB249EE20B3BD5D0102E06A300373B7C9DE8673937D3BDB1497AB'
        },
        @{
            Name = 'VCRUNTIME140.dll'
            Hash = '94F8E3471DE34D1C659B968266F66BD2D4A10A65254D1050A00FF641593CBEB2'
        },
        @{
            Name = 'VCRUNTIME140_1.dll'
            Hash = 'AEE5DE9A28994D65C0EBB1677CE5868711AEC6158920E768A2FA19D0A7192FA1'
        }
    )
} else {
    # Nuke 15.1v4: MSVC 14.36.32532.
    @(
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
}

foreach ($runtimeFile in $runtimeFiles) {
    $target = Join-Path $testDirectory $runtimeFile.Name
    if ($RuntimeDirectory) {
        Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $runtimeFile.Name) -Destination $target -Force
    } else {
        $url = 'https://msdl.microsoft.com/download/symbols/' + $runtimeFile.Name + '/' +
               $runtimeFile.Key + '/' + $runtimeFile.Name
        Invoke-WebRequest -Uri $url -OutFile $target
    }
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $runtimeFile.Hash) {
        throw ('Unexpected Microsoft runtime file ' + $runtimeFile.Name)
    }
}

$legacyTest = Join-Path $testDirectory 'chromatron-legacy-tests.exe'
Copy-Item -LiteralPath $testSource -Destination $legacyTest -Force
& $legacyTest
if ($LASTEXITCODE -ne 0) {
    throw ('Tests with the ' + $RuntimeProfile + ' MSVC runtime failed, process exit ' + $LASTEXITCODE)
}
