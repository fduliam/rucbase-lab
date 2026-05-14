cmake_minimum_required(VERSION 3.16)

# TPC-C 加载器与驱动器：纯 socket client，不依赖 RucBase 服务端代码
add_executable(tpcc_loader tpcc_loader.cpp)
target_link_libraries(tpcc_loader pthread)

add_executable(tpcc_driver tpcc_driver.cpp)
target_link_libraries(tpcc_driver pthread)
