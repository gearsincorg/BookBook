cmake_minimum_required(VERSION 3.16)

# Optional developer secrets (Wi-Fi etc.), never committed.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets/sdkconfig.secrets")
    list(APPEND SDKCONFIG_DEFAULTS "secrets/sdkconfig.secrets")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bookbook)
