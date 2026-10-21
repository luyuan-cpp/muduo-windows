set(base_SRCS
  AsyncLogging.cc
  CountDownLatch.cc
  Date.cc
  Exception.cc
  FileUtil.cc
  LogFile.cc
  Logging.cc
  LogStream.cc
  ProcessInfo.cc
  Timestamp.cc
  TimeZone.cc
  ThreadPool.cc
  CurrentThread.cc
  CrossPlatformAdapterFunction.cc
  Thread.cc
  )
include_directories(${PROJECT_SOURCE_DIR}/contrib/windows/include)
add_library(muduo_base ${base_SRCS})

if (WIN32)
  target_compile_options(muduo_base PUBLIC /FI"muduo/base/CrossPlatformAdapterFunction.h")
endif ()

 target_link_libraries(muduo_base gtest)
if (UNIX)
 target_link_libraries(muduo_base pthread rt)
endif ()

install(TARGETS muduo_base DESTINATION lib)

file(GLOB HEADERS "*.h")
install(FILES ${HEADERS} DESTINATION include/muduo/base)

if(MUDUO_BUILD_TESTS)
  add_subdirectory(tests)
endif()
