cmake_minimum_required(VERSION 3.21)
project(steamutils)

set(CMAKE_CXX_STANDARD 20)

add_library(steamutils SHARED
        source/api/steam_api.cpp
        source/api/steam_grid_api.cpp

        source/file_formats/vdf.cpp
        source/file_formats/keyvalues.cpp

        source/helpers/file.cpp
        source/helpers/utils.cpp
        source/helpers/net.cpp
)

set_target_properties(steamutils
        PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_link_libraries(steamutils PUBLIC fmt::fmt nlohmann_json curl)

target_include_directories(steamutils PUBLIC include external/fmt/include)
