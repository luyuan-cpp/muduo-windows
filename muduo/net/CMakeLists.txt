include(CheckFunctionExists)

check_function_exists(acppept4 HAVE_AcppEPT4)
if(NOT HAVE_AcppEPT4)
  set_source_files_properties(SocketsOps.cc PROPERTIES COMPILE_FLAGS "-DNO_AcppEPT4")
endif()

set(net_SRCS
  Acceptor.cc
  Buffer.cc
  Channel.cc
  Connector.cc
  EventLoop.cc
  EventLoopThread.cc
  EventLoopThreadPool.cc
  InetAddress.cc
  Poller.cc
  poller/DefaultPoller.cc
  poller/EPollPoller.cc
  poller/PollPoller.cc
  Socket.cc
  SocketsOps.cc
  TcpClient.cc
  TcpConnection.cc
  TcpServer.cc
  Timer.cc
  TimerQueue.cc
  )
if(WIN32)
  list(APPEND net_SRCS WindowsWakeup.cc)
endif()
include_directories(${PROJECT_SOURCE_DIR}/contrib/windows/include)
add_library(muduo_net ${net_SRCS})
if (WIN32)
  target_compile_options(muduo_net PUBLIC /FI"muduo/base/CrossPlatformAdapterFunction.h")
endif ()
target_link_libraries(muduo_net muduo_base)

install(TARGETS muduo_net DESTINATION lib)

set(HEADERS
  Buffer.h
  Callbacks.h
  Channel.h
  Endian.h
  EventLoop.h
  EventLoopThread.h
  EventLoopThreadPool.h
  InetAddress.h
  TcpClient.h
  TcpConnection.h
  TcpServer.h
  TimerId.h
  )
install(FILES ${HEADERS} DESTINATION include/muduo/net)

if(MUDUO_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(MUDUO_BUILD_PROTOBUF)
if (MSVC)
  add_subdirectory(protobuf)
  add_subdirectory(protorpc)
else(MSVC)
if(PROTOBUF_FOUND)
  add_subdirectory(protobuf)
  add_subdirectory(protorpc)
else()
  add_subdirectory(protobuf EXCLUDE_FROM_ALL)
  add_subdirectory(protorpc EXCLUDE_FROM_ALL)
endif()
endif()
endif()
