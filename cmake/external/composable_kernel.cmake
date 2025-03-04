set(composable_kernel_URL https://github.com/ROCmSoftwarePlatform/composable_kernel.git)
set(composable_kernel_TAG 8ee36118be9b19b15c2471bffeeeb624afb14044) # 2022-11-01 00:24:25 +0800

set(PATCH ${PROJECT_SOURCE_DIR}/patches/composable_kernel/Fix_Clang_Build.patch)

include(FetchContent)
FetchContent_Declare(composable_kernel
  GIT_REPOSITORY ${composable_kernel_URL}
  GIT_TAG        ${composable_kernel_TAG}
  PATCH_COMMAND  git apply --reverse --check ${PATCH} || git apply --ignore-space-change --ignore-whitespace ${PATCH}
)

FetchContent_GetProperties(composable_kernel)
if(NOT composable_kernel_POPULATED)
  FetchContent_Populate(composable_kernel)
  set(BUILD_DEV OFF CACHE BOOL "Disable -Weverything, otherwise, error: 'constexpr' specifier is incompatible with C++98 [-Werror,-Wc++98-compat]" FORCE)
  add_subdirectory(${composable_kernel_SOURCE_DIR} ${composable_kernel_BINARY_DIR} EXCLUDE_FROM_ALL)

  add_library(onnxruntime_composable_kernel_includes INTERFACE)
  target_include_directories(onnxruntime_composable_kernel_includes INTERFACE
    ${composable_kernel_SOURCE_DIR}/include
    ${composable_kernel_SOURCE_DIR}/library/include)
endif()
