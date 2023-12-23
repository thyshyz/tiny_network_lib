# 加载子目录
set(
    echoServer.cc
)

add_executable(echoServer echoServer.cc)


set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/example)

target_link_libraries(echoServer tiny_network)

