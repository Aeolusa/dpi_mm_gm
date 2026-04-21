ref_model/
├── CMakeLists.txt
├── include/
│   ├── types.h                    // 基础类型 & 枚举
│   ├── transaction.h              // Transaction 结构体
│   ├── shadow_memory.h            // 影子存储器
│   ├── consistency_checker.h      // 一致性检查引擎
│   ├── transaction_manager.h      // 事务管理器（顶层）
│   └── error_reporter.h           // 错误格式化 & 日志
├── src/
│   ├── shadow_memory.cpp
│   ├── consistency_checker.cpp
│   ├── transaction_manager.cpp
│   ├── error_reporter.cpp
│   └── dpi_interface.cpp          // DPI-C 胶水层
├── test/
│   ├── unit_test_shadow_mem.cpp   // GoogleTest 单元测试
│   ├── unit_test_checker.cpp
│   └── integration_test.cpp       // 多master场景集成测试
└── scripts/
    └── compile_dpi.sh             // VCS/Xcelium 编译脚本
