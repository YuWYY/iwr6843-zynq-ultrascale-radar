param(
    [Parameter(Mandatory=$true)][string]$Output,
    [Parameter(Mandatory=$true)][string]$Fixture,
    [Parameter(Mandatory=$true)][string]$Models,
    [Parameter(Mandatory=$true)][string]$Vendor,
    [string]$Vivado='C:/Xilinx/2025.1/Vivado'
)
$ErrorActionPreference='Stop'
$dspSource = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (Test-Path -LiteralPath $Output) { throw 'Use a new evidence directory.' }
$dspOutput = [IO.Path]::GetFullPath($Output).Replace('\','/')
$dspVendor = (Resolve-Path -LiteralPath $Vendor).Path.Replace('\','/')
New-Item -ItemType Directory -Path "$dspOutput/results" -Force | Out-Null
$dspFiles = @(Get-ChildItem -LiteralPath "$dspSource/rtl/dsp" -File)
$dspFiles += @(Get-Item -LiteralPath "$PSScriptRoot/tb_dsp_batch.sv","$PSScriptRoot/fft_vendor_axis.sv","$PSScriptRoot/fft_vendor_dpi.c","$PSScriptRoot/generate_batch_reference.py","$PSScriptRoot/check_batch.py")
$dspHashes = @($dspFiles | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
$dspHashes | ConvertTo-Json | Set-Content -LiteralPath "$dspOutput/source_manifest.json" -Encoding utf8
python "$PSScriptRoot/generate_batch_reference.py" "$dspOutput/reference" $Fixture $Models $dspVendor *> "$dspOutput/reference.log"
if ($LASTEXITCODE -ne 0) { throw 'Reference generation failed.' }
Set-Content -LiteralPath "$dspOutput/run_all.tcl" -Value "run all`nquit" -Encoding ascii
$dspRtl = (@(Get-ChildItem -LiteralPath "$dspSource/rtl/dsp" -Filter '*.sv' -File) | ForEach-Object {'"'+$_.FullName.Replace('\','/')+'"'}) -join ' '
$dspCommands = @(
 '@echo off',
 "call `"$Vivado/bin/xsc.bat`" `"$PSScriptRoot/fft_vendor_dpi.c`" --gcc_compile_options `"-I$dspVendor`" -o vendor_fft",
 "call `"$Vivado/bin/xvlog.bat`" --relax -sv $dspRtl `"$PSScriptRoot/fft_vendor_axis.sv`" `"$PSScriptRoot/tb_dsp_batch.sv`"",
 "call `"$Vivado/bin/xelab.bat`" work.tb_dsp_batch -L xpm --debug off --O3 --snapshot dsp_batch --sv_root . --sv_lib vendor_fft",
 "set RADAR_XFFT_CMODEL_DLL=$dspVendor/libIp_xfft_v9_1_bitacc_cmodel.dll",
 "call `"$Vivado/bin/xsim.bat`" dsp_batch -tclbatch `"$dspOutput/run_all.tcl`" -testplusarg `"REFERENCE=$dspOutput/reference`" -testplusarg `"OUT=$dspOutput/results`" -log `"$dspOutput/run.log`" -wdb `"$dspOutput/run.wdb`"",
 'exit /b %errorlevel%'
)
Set-Content -LiteralPath "$dspOutput/run.cmd" -Value $dspCommands -Encoding ascii
Push-Location -LiteralPath $dspOutput
try { & "$dspOutput/run.cmd" *> "$dspOutput/launcher.log" } finally { Pop-Location }
$dspLog=Get-Content -LiteralPath "$dspOutput/launcher.log" -Raw
if ($dspLog -match '(?im)^\s*(ERROR:|Fatal:)' -or $dspLog -notmatch 'PASS dsp_batch64') { throw 'Native co-simulation failed; evidence retained.' }
python "$PSScriptRoot/check_batch.py" $dspOutput
if ($LASTEXITCODE -ne 0) { throw 'Independent batch comparison failed.' }
$dspAfter = @($dspFiles | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
if (Compare-Object $dspHashes $dspAfter -Property Path,Hash) { throw 'Sources changed during verification.' }
