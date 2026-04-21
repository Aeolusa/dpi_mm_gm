cmake_minimum_required(VERSION 3.14)
project(MemConsistencyRefModel CXX)
set(CMAKE_CXX_STANDARD 17)

# 头文件
include_directories(include)

# 编译为共享库（供仿真器DPI加载）
add_library(refmodel SHARED
    src/shadow_memory.cpp
    src/consistency_checker.cpp
    src/transaction_manager.cpp
    src/dpi_interface.cpp
)

# 单元测试
enable_testing()
find_package(GTest REQUIRED)
add_executable(run_tests
    test/unit_test_shadow_mem.cpp
    test/unit_test_checker.cpp
    test/integration_test.cpp
)
target_link_libraries(run_tests refmodel GTest::gtest_main)
add_test(NAME AllTests COMMAND run_tests)
