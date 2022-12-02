# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

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