// ============================================================
// file: src/refmodel_config.cc
// INI 风格配置文件解析器
//
// 格式规则:
//   - 每行 key = value
//   - # 开头为注释行
//   - 空行忽略
//   - key/value 前后空白自动去除
//   - 未识别的 key 静默忽略
// ============================================================
#include "refmodel_config.h"
#include "logger.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

// ---- 辅助: 去除字符串首尾空白 ----
static std::string trim(const std::string& s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(static_cast<unsigned char>(*start)))
        ++start;
    auto end = s.end();
    while (end != start && std::isspace(static_cast<unsigned char>(*(end - 1))))
        --end;
    return std::string(start, end);
}

// ---- 辅助: 十六进制字符串 → uint64_t ----
static uint64_t parse_hex(const std::string& s) {
    uint64_t val = 0;
    std::istringstream iss(s);
    iss >> std::hex >> val;
    return val;
}

RefModelConfig load_config(const std::string& filepath) {
    RefModelConfig cfg;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        // 文件不存在，返回默认配置
        return cfg;
    }

    cfg.loaded_from_file = true;
    std::string line;
    int line_num = 0;

    while (std::getline(file, line)) {
        line_num++;
        line = trim(line);

        // 跳过空行和注释
        if (line.empty() || line[0] == '#')
            continue;

        // 解析 key = value
        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            LOG_DEBUG("[CONFIG] Line " << line_num << ": no '=' found, skipped\n");
            continue;
        }

        std::string key   = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));

        // 匹配 key
        if (key == "strict_mode") {
            cfg.strict_mode = std::stoi(value);
        } else if (key == "log_level") {
            cfg.log_level = std::stoi(value);
        } else if (key == "log_file") {
            cfg.log_file = value;
        } else if (key == "preload_hex_file") {
            cfg.preload_hex_file = value;
        } else if (key == "preload_hex_width") {
            cfg.preload_hex_width = std::stoi(value);
        } else if (key == "addr_filter_mask") {
            cfg.addr_filter_mask = parse_hex(value);
        } else if (key == "enable_soft_ctrl") {
            cfg.enable_soft_ctrl = (std::stoi(value) != 0);
        } else {
            LOG_DEBUG("[CONFIG] Line " << line_num << ": unknown key '" << key << "', skipped\n");
        }
    }

    return cfg;
}
