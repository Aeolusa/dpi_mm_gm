cmake_minimum_required(VERSION 3.14)
project(MemConsistencyRefModel CXX)
set(CMAKE_CXX_STANDARD 17)

# 头文件
include_directories(code/cc/include)

# 编译为共享库（供仿真器DPI加载）
add_library(refmodel SHARED
    code/cc/src/shadow_memory.cc
    code/cc/src/consistency_check.cc
    code/cc/src/chi_transaction_manager.cc
    code/cc/src/utils.cc
    code/cc/src/mm_gm_dpi.cc
)

# 单元测试
enable_testing()
find_package(GTest REQUIRED)
add_executable(run_tests
    code/cc/test/unit_test_shadow_mem.cc
    code/cc/test/unit_test_checker.cc
    code/cc/test/integration_test.cc
)
target_link_libraries(run_tests refmodel GTest::gtest_main)
add_test(NAME AllTests COMMAND run_tests)
