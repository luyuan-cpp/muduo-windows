cmake_minimum_required(VERSION 3.5...3.31)

if(POLICY CMP0048)
  cmake_policy(SET CMP0048 NEW)
endif()
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

project(muduo C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MUDUO_BUILD_PROTOBUF "构建可选 protobuf 编解码、RPC 和相关示例" ON)
option(MUDUO_BUILD_TESTS "构建 Muduo 测试" ON)

if(MSVC)
  # 依赖和本库统一使用 /MD(d)，在 FetchContent 创建任何目标前固定运行库。
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  add_compile_options(/std:c++latest /utf-8)
endif()

# Keep FetchContent cache stable across different build directories.
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.fc-cache" CACHE PATH "FetchContent shared cache" FORCE)
include(FetchContent)

# Release tags (can be overridden with -Dxxx_TAG=...)
set(BOOST_TAG "boost-1.90.0" CACHE STRING "Boost release tag")
set(GTEST_TAG "v1.14.0" CACHE STRING "googletest release tag")
set(PROTOBUF_TAG "v3.21.12" CACHE STRING "protobuf release tag")
set(ZLIB_TAG "v1.3.1" CACHE STRING "zlib release tag")

set(BOOST_LOCAL_DIR "${CMAKE_SOURCE_DIR}/boost")
set(GTEST_LOCAL_DIR "${CMAKE_SOURCE_DIR}/googletest")
set(PROTOBUF_LOCAL_DIR "${CMAKE_SOURCE_DIR}/protobuf")
set(ZLIB_LOCAL_DIR "${CMAKE_SOURCE_DIR}/zlib")

if(EXISTS "${BOOST_LOCAL_DIR}")
  message(STATUS "Using local boost source: ${BOOST_LOCAL_DIR}")
  FetchContent_Declare(boost SOURCE_DIR "${BOOST_LOCAL_DIR}")
else()
  FetchContent_Declare(boost
    URL https://archives.boost.io/release/1.90.0/source/boost_1_90_0.zip
  )
endif()

if(EXISTS "${GTEST_LOCAL_DIR}/CMakeLists.txt")
  message(STATUS "Using local googletest source: ${GTEST_LOCAL_DIR}")
  FetchContent_Declare(googletest SOURCE_DIR "${GTEST_LOCAL_DIR}")
else()
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG ${GTEST_TAG}
    GIT_SHALLOW TRUE
  )
endif()

if(MUDUO_BUILD_PROTOBUF)
if(EXISTS "${PROTOBUF_LOCAL_DIR}/CMakeLists.txt")
  message(STATUS "Using local protobuf source: ${PROTOBUF_LOCAL_DIR}")
  FetchContent_Declare(protobuf SOURCE_DIR "${PROTOBUF_LOCAL_DIR}")
else()
  FetchContent_Declare(protobuf
    GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
    GIT_TAG ${PROTOBUF_TAG}
    GIT_SHALLOW TRUE
  )
endif()
endif()

if(EXISTS "${ZLIB_LOCAL_DIR}/CMakeLists.txt")
  message(STATUS "Using local zlib source: ${ZLIB_LOCAL_DIR}")
  FetchContent_Declare(zlib SOURCE_DIR "${ZLIB_LOCAL_DIR}")
else()
  FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG ${ZLIB_TAG}
    GIT_SHALLOW TRUE
  )
endif()

# Protobuf defaults bring its own gmock/gtest tests, which conflicts with
# the separately fetched googletest in this workspace.
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(MSVC)
  # Keep protobuf CRT consistent with this workspace (/MD).
  set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
endif()

# Prevent GoogleTest from overriding our compiler/linker options
# when building with Visual Studio
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(boost googletest zlib)
if(MUDUO_BUILD_PROTOBUF)
  FetchContent_MakeAvailable(protobuf)
endif()

enable_testing()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()

if (MSVC)

  # Build with multiple processes
  add_definitions(/MP)
  add_definitions( /wd4068 /wd4996 /wd4244)
  # Allow big object
  add_definitions(/bigobj)
else (MSVC)

  set(CXX_FLAGS
  -g
  # -DVALGRIND
  # -DMUDUO_STD_STRING
  -DCHECK_PTHREAD_RETURN_VALUE
  -D_FILE_OFFSET_BITS=64
  -Wall
  -Wextra
  -Werror
  -Wconversion
  -Wno-unused-parameter
  #-Wold-style-cast
  -Woverloaded-virtual
  -Wpointer-arith
  -Wshadow
  -Wwrite-strings
  -march=native
  -rdynamic
  -pthread 
  -lpthread 
  -D_GLIBCXX_USE_DEPRECATED=0
  )
  
if(CMAKE_BUILD_BITS EQUAL 64)
  list(APPEND CXX_FLAGS "-m64")
endif()

string(REPLACE ";" " " CMAKE_CXX_FLAGS "${CXX_FLAGS}")
set(CMAKE_CXX_COMPILER "g++")
set(CMAKE_CXX_FLAGS_DEBUG "-O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -finline-limit=1000 -DNDEBUG")
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)
endif (MSVC)

include_directories(contrib/boost/boost)
include_directories(contrib/windows/include)
include_directories(${boost_SOURCE_DIR})
include_directories(${googletest_SOURCE_DIR}/googletest/include)
include_directories(${googletest_SOURCE_DIR}/googlemock/include)
if(MUDUO_BUILD_PROTOBUF)
  include_directories(${protobuf_SOURCE_DIR}/src)
endif()
include_directories(muduo/net/protorpc)
include_directories(${zlib_SOURCE_DIR})
include_directories("${zlib_BINARY_DIR}")

include_directories(${PROJECT_SOURCE_DIR})

string(TOUPPER ${CMAKE_BUILD_TYPE} BUILD_TYPE)
message(STATUS "CXX_FLAGS = " ${CMAKE_CXX_FLAGS} " " ${CMAKE_CXX_FLAGS_${BUILD_TYPE}})

add_subdirectory(muduo/base)
add_subdirectory(muduo/net)
if(MUDUO_BUILD_PROTOBUF AND NOT CMAKE_BUILD_NO_EXAMPLES)
  add_subdirectory(examples/protobuf)
endif()

# Merge muduo_base.lib + muduo_net.lib into a single muduo.lib
if(MSVC)
  set(muduo_combined_output "${CMAKE_BINARY_DIR}/lib/$<CONFIG>/muduo.lib")
  add_custom_target(muduo ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
    COMMAND "${CMAKE_AR}" /NOLOGO
            "/OUT:${muduo_combined_output}"
            "$<TARGET_FILE:muduo_base>"
            "$<TARGET_FILE:muduo_net>"
    DEPENDS muduo_base muduo_net
    COMMENT "Merging muduo_base.lib and muduo_net.lib into muduo.lib"
    VERBATIM
  )
endif()
