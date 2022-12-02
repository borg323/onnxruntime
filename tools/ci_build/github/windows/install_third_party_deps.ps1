# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

 param (
    [string]$cpu_arch = "x64",
    [string]$build_config = "RelWithDebInfo",
    [string]$install_prefix
 )
 
 function Install-Pybind {

    param (
        [Parameter(Mandatory)][string]$cmake_path,
        [Parameter(Mandatory)][string]$build_config,
        [Parameter(Mandatory)][string[]]$cmake_extra_args   
    )

    pushd .

    $url='https://github.com/pybind/pybind11/archive/refs/tags/v2.10.1.zip'
    Write-Host "Downloading pybind11 from $url"
    Invoke-WebRequest -Uri $url -OutFile pybind11.zip
    7z x pybind11.zip
    cd pybind11-2.10.1
    mkdir build
    cd build
    [string[]]$cmake_args = "..", "-DCMAKE_INSTALL_PREFIX=$install_prefix", "-DBUILD_TESTING=OFF"
    $cmake_args += $cmake_extra_args
    Start-Process -FilePath $cmake_path -ArgumentList $cmake_args -NoNewWindow -Wait
    $cmake_args = "--build", ".",  "--parallel", "--config", $build_config, "--target", "INSTALL"
    Start-Process -FilePath $cmake_path -ArgumentList $cmake_args -NoNewWindow -Wait
    popd
}

function Install-Protobuf {

    param (
        [Parameter(Mandatory)][string]$cmake_path,
        [Parameter(Mandatory)][string]$build_config,
        [Parameter(Mandatory)][string]$protobuf_version,
        [Parameter(Mandatory)][string[]]$cmake_extra_args   
    )

    pushd .

    $url="https://github.com/protocolbuffers/protobuf/releases/download/v$protobuf_version/protobuf-cpp-$protobuf_version.zip"
    Write-Host "Downloading protobuf from $url"
    Invoke-WebRequest -Uri $url -OutFile protobuf_src.zip
    7z x protobuf_src.zip
    cd protobuf-$protobuf_version
    Get-Content $Env:BUILD_SOURCESDIRECTORY\cmake\patches\protobuf\protobuf_cmake.patch | &'C:\Program Files\Git\usr\bin\patch.exe' --binary --ignore-whitespace -p1

    [string[]]$cmake_args = "cmake", "-DCMAKE_BUILD_TYPE=$build_config", "-Dprotobuf_BUILD_TESTS=OFF", "-DBUILD_SHARED_LIBS=OFF", "-DCMAKE_PREFIX_PATH=$install_prefix",  "-DCMAKE_INSTALL_PREFIX=$install_prefix", "-Dprotobuf_MSVC_STATIC_RUNTIME=OFF"
    $cmake_args += $cmake_extra_args
    
    Start-Process -FilePath $cmake_path -ArgumentList $cmake_args -NoNewWindow -Wait
    $cmake_args = "--build", ".",  "--parallel", "--config", $build_config, "--target", "INSTALL"
    Start-Process -FilePath $cmake_path -ArgumentList $cmake_args -NoNewWindow -Wait
    popd
}

function Install-ONNX {

    param (
        [Parameter(Mandatory)][string]$build_config,
        [Parameter(Mandatory)][string]$protobuf_version
    )

    pushd .

    python -m pip install -q setuptools wheel numpy protobuf==$protobuf_version pybind11
    $onnx_commit_id="5a5f8a5935762397aa68429b5493084ff970f774"
    $url="https://github.com/onnx/onnx/archive/$onnx_commit_id.zip"
    Write-Host "Downloading onnx from $url"
    Invoke-WebRequest -Uri $url -OutFile onnx.zip
    7z x onnx.zip
    cd "onnx-$onnx_commit_id"
    $Env:ONNX_ML=1
    if($build_config -eq 'Debug'){
       $Env:DEBUG='1'
    }
    $Env:CMAKE_ARGS="-DONNX_USE_PROTOBUF_SHARED_LIBS=OFF -DProtobuf_USE_STATIC_LIBS=ON -DONNX_USE_LITE_PROTO=OFF"
    python setup.py bdist_wheel
    python -m pip uninstall -y onnx -qq
    Get-ChildItem -Path dist/*.whl | foreach {pip --disable-pip-version-check install --upgrade $_.fullname}
    popd
}

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