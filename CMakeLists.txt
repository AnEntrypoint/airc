cmake_minimum_required(VERSION 3.21)
project(ccbridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(Boost 1.84 REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)

file(GLOB LIBVTERM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvterm/src/*.c)
add_library(vterm STATIC ${LIBVTERM_SOURCES})
target_include_directories(vterm
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvterm/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvterm/src)
if(MSVC)
    target_compile_options(vterm PRIVATE /W0)
else()
    target_compile_options(vterm PRIVATE -w)
endif()

file(GLOB_RECURSE CCB_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER CCB_SOURCES EXCLUDE REGEX "/src/main\\.cpp$")

add_library(ccb_core OBJECT ${CCB_SOURCES})
target_include_directories(ccb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ccb_core PUBLIC
    vterm
    Boost::headers
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    SQLite::SQLite3
    Threads::Threads)

add_executable(ccbridge ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_compile_definitions(ccbridge PRIVATE CCB_VERSION="${PROJECT_VERSION}")
target_link_libraries(ccbridge PRIVATE ccb_core)

enable_testing()
add_executable(claude_channel_test EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/claude_channel_test.cpp)
target_link_libraries(claude_channel_test PRIVATE ccb_core)
add_test(NAME claude_channel COMMAND claude_channel_test)

foreach(target ccb_core ccbridge claude_channel_test)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- /bigobj)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

install(TARGETS ccbridge RUNTIME DESTINATION bin)
install(DIRECTORY templates web DESTINATION share/ccbridge)
