# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

 param (
    [string]$cpu_arch = "x64",
    [string]$build_config = "RelWithDebInfo",
    [string]$install_prefix
 )

. "$PSScriptRoot\helpers.ps1"

$ErrorActionPreference = "Stop"

$Env:Path = "$install_prefix\bin;C:\Program Files\7-Zip;" + $env:Path
$Env:MSBUILDDISABLENODEREUSE=1

New-Item -Path "$install_prefix" -ItemType Directory -Force

$compile_flags = '/guard:cf /Qspectre /std:c++17 /DWIN32 /D_WINDOWS /Gw /GL /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000'
$linker_flags=@('/guard:cf')
[string[]]$cmake_extra_args="`"-DCMAKE_CXX_FLAGS=$compile_flags /EHsc`" ", "`"-DCMAKE_C_FLAGS=$compile_flags`""
if($cpu_arch -eq 'x86'){
  Write-Host "Build for x86"
  $cmake_extra_args +=  "-A", "Win32", "-T", "host=x64"
  $linker_flags += '/machine:x86'
} else {
  Write-Host "Build for $cpu_arch"
  $linker_flags += '/machine:x64'
}


$cmake_extra_args += "-DCMAKE_EXE_LINKER_FLAGS=`"$linker_flags`""

$cmake_command = Get-Command -CommandType Application cmake

$cmake_path = $cmake_command.Path



Install-Pybind -cmake_path $cmake_path -build_config $build_config  -cmake_extra_args $cmake_extra_args

$protobuf_version="3.18.3"
Install-Protobuf -cmake_path $cmake_path -build_config $build_config -protobuf_version $protobuf_version -cmake_extra_args $cmake_extra_args
Install-ONNX -build_config $build_config -protobuf_version $protobuf_version