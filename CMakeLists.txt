# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/version.cmake)
if(IDF_VERSION_MAJOR AND "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_LESS "6.1")
    message(STATUS "IDF ${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR} (<6.1): 启用 SECP192K1 workaround")
    add_compile_definitions(
        TF_PSA_CRYPTO_ALLOW_REMOVED_MECHANISMS
        PSA_WANT_ECC_SECP_K1_192=1
    )
endif()

# 使用默认配置
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
)
# cs=1：IN=16KB 容纳 COS 证书；须在 project() 前加入，供首次生成 sdkconfig
if((DEFINED ENV{BUILD_IOT_CLOUD_STORAGE} AND "$ENV{BUILD_IOT_CLOUD_STORAGE}" STREQUAL "1")
   OR (DEFINED BUILD_IOT_CLOUD_STORAGE AND "${BUILD_IOT_CLOUD_STORAGE}" STREQUAL "1"))
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.cs1")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(tc-iot-video-rtc)

# SPIFFS 镜像：将 spiffs_data/ 目录打包烧录到 "storage" 分区
spiffs_create_partition_image(storage spiffs_data FLASH_IN_PROJECT)
