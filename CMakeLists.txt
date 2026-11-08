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

add_executable(ccbridge ${CCB_SOURCES})
target_include_directories(ccbridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ccbridge PRIVATE CCB_VERSION="${PROJECT_VERSION}")
target_link_libraries(ccbridge PRIVATE
    vterm
    Boost::headers
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    SQLite::SQLite3
    Threads::Threads)

if(MSVC)
    target_compile_options(ccbridge PRIVATE /W4 /utf-8 /permissive-)
    target_compile_definitions(ccbridge PRIVATE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
else()
    target_compile_options(ccbridge PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS ccbridge RUNTIME DESTINATION bin)
install(DIRECTORY templates web DESTINATION share/ccbridge)
