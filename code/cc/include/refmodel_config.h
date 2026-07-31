// ============================================================
// file: include/refmodel_config.h
// RefModel 运行时配置
//   - 支持从 INI 风格配置文件加载
//   - 缺失字段使用默认值
//   - 文件不存在时静默返回默认配置
// ============================================================
#pragma once

#include <cstdint>
#include <string>

struct RefModelConfig {
    int         strict_mode       = 1;     // 0=关闭, 1=开启
    int         log_level         = 1;     // 0=QUIET, 1=ERROR, 2=INFO, 3=DEBUG
    std::string log_file;                  // 为空不写文件
    std::string preload_hex_file;          // 为空不预加载
    int         preload_hex_width = 8;     // hex 每行数据宽度（字节数）
    uint64_t    addr_filter_mask  = 0;     // (addr & mask) != 0 → 软过滤
    bool        enable_soft_ctrl  = true;  // 是否启用 soft_ctrl

    bool        loaded_from_file  = false; // 是否成功加载了配置文件
};

// 解析 INI 配置文件，缺失字段使用默认值。
// 文件不存在时返回默认配置（loaded_from_file = false）。
RefModelConfig load_config(const std::string& filepath);
