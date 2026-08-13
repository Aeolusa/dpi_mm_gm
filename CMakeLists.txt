cmake_minimum_required(VERSION 3.14)
project(MemConsistencyRefModel CXX)
set(CMAKE_CXX_STANDARD 17)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# 头文件
include_directories(code/cc/include)

# 编译为共享库（供仿真器DPI加载）
add_library(refmodel SHARED
    code/cc/src/shadow_memory.cc
    code/cc/src/consistency_check.cc
    code/cc/src/chi_transaction_manager.cc
    code/cc/src/refmodel_config.cc
    code/cc/src/utils.cc
    code/cc/src/logger.cc
    code/cc/src/mm_gm_dpi.cc
)

# 单元测试
enable_testing()
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(refmodel_tests
    code/cc/test/unit_test_shadow_mem.cc
    code/cc/test/unit_test_checker.cc
    code/cc/test/integration_test.cc
    code/cc/test/test_addr_truncation.cc
)
target_link_libraries(refmodel_tests refmodel gtest_main)
add_test(NAME AllTests COMMAND refmodel_tests)

# 独立地址截断诊断测试（无 GTest 依赖，可在内网服务器编译）
add_executable(addr_test
    code/cc/test/standalone_addr_test.cc
)
target_link_libraries(addr_test refmodel)
add_test(NAME AddrTruncTest COMMAND addr_test)
