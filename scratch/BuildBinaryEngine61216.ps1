# Copyright (C) 2026 batuhan.
# GNU GPL version 3 only, with LICENSE-7ZIP-EXCEPTION.txt.
$ErrorActionPreference='Stop'
$binarySourceRoot=Split-Path -Parent $PSScriptRoot
$binaryEngineOut=Join-Path $binarySourceRoot 'src\binary_engine\build'
if(-not(Test-Path -LiteralPath $binaryEngineOut)){New-Item -ItemType Directory -Path $binaryEngineOut | Out-Null}
$binaryCompiler='C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\Roslyn\csc.exe'
$binaryReferencesRoot='C:\Program Files (x86)\dotnet\shared\Microsoft.NETCore.App\8.0.22'
$binaryReferences=@(Get-ChildItem -LiteralPath $binaryReferencesRoot -Filter '*.dll' -File | ForEach-Object {try{$null=[Reflection.AssemblyName]::GetAssemblyName($_.FullName);'/r:'+ $_.FullName}catch{}})
$binaryCommon=@('/nologo','/noconfig','/nostdlib+','/target:library','/platform:x86','/unsafe+','/langversion:latest','/optimize+','/deterministic+')
$binaryCoreSources=@(Get-ChildItem -LiteralPath (Join-Path $binarySourceRoot 'research\coreextensions-binary-audit\CoreExtensions') -Filter '*.cs' -File -Recurse | Where-Object {$_.Name -ne 'ASMBuilder.cs'} | ForEach-Object FullName)
$binaryNikkiSources=@(Get-ChildItem -LiteralPath (Join-Path $binarySourceRoot 'research\nikki-binary-audit\Nikki') -Filter '*.cs' -File -Recurse | Where-Object {$_.FullName -notmatch '\\Utils\\Interop.cs$' -and $_.FullName -notmatch '\\Support.Underground2\\Framework\\DBModelPartManager.cs$' -and $_.FullName -notmatch '\\Support.Underground2\\Attributes\\(?:StringAttribute|CustomAttribute).cs$' -and $_.FullName -notmatch '\\Support.Underground2\\Parts\\CarParts\\CPStruct.cs$'} | ForEach-Object FullName)
$binaryNikkiSources+=@(Get-ChildItem -LiteralPath (Join-Path $binarySourceRoot 'src\binary_engine\nikki_safe') -Filter '*.cs' -File | ForEach-Object FullName)
function Invoke-BinaryCompiler([string[]]$binaryArguments){
    $binaryResponse=Join-Path $binaryEngineOut 'compiler.rsp'
    [IO.File]::WriteAllLines($binaryResponse,@($binaryArguments | ForEach-Object {'"'+$_+'"'}),[Text.UTF8Encoding]::new($false))
    & $binaryCompiler /noconfig ('@'+$binaryResponse)
    if($LASTEXITCODE -ne 0){throw 'Binary engine compiler failed'}
}
Invoke-BinaryCompiler (@($binaryCommon)+@('/out:'+(Join-Path $binaryEngineOut 'CoreExtensions.dll'))+$binaryReferences+$binaryCoreSources)
if($LASTEXITCODE -ne 0){throw 'CoreExtensions source build failed'}
Invoke-BinaryCompiler (@($binaryCommon)+@('/out:'+(Join-Path $binaryEngineOut 'Nikki.dll'))+$binaryReferences+@('/r:'+(Join-Path $binaryEngineOut 'CoreExtensions.dll'))+$binaryNikkiSources+@(Join-Path $binarySourceRoot 'src\binary_engine\SafeCompression.cs'))
if($LASTEXITCODE -ne 0){throw 'Bounded Nikki source build failed'}
$binaryOwnSources=@(Get-ChildItem -LiteralPath (Join-Path $binarySourceRoot 'src\binary_engine') -Filter '*.cs' -File | Where-Object {$_.Name -ne 'SafeCompression.cs'} | ForEach-Object FullName)
$binaryAppCommon=@('/nologo','/nostdlib+','/target:exe','/platform:x86','/langversion:latest','/optimize+','/deterministic+')
$binaryAppCommon+=('/out:'+(Join-Path $binaryEngineOut 'Undercoop.Binary.dll'))
$binaryAppCommon+=('/r:'+(Join-Path $binaryEngineOut 'CoreExtensions.dll'))
$binaryAppCommon+=('/r:'+(Join-Path $binaryEngineOut 'Nikki.dll'))
Invoke-BinaryCompiler ($binaryAppCommon+$binaryReferences+$binaryOwnSources)
if($LASTEXITCODE -ne 0){throw 'Binary data adapter build failed'}
Get-FileHash -LiteralPath (Join-Path $binaryEngineOut 'Undercoop.Binary.dll'),(Join-Path $binaryEngineOut 'Nikki.dll'),(Join-Path $binaryEngineOut 'CoreExtensions.dll') -Algorithm SHA256 | Select-Object Path,Hash
