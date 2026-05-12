// ============================================================
// file: include/logger.h
// 统一日志控制系统
//   - 支持 4 级日志级别: QUIET / ERROR / INFO / DEBUG
//   - 默认 ERROR 级别：仅输出数据比对失败等错误信息
//   - 支持可选文件输出（所有日志同时写入文件）
//   - 单例模式，全局可用
// ============================================================
#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>

// ---------- 日志级别 ----------
enum class LogLevel : uint8_t {
    QUIET  = 0,   // 完全静默，不输出任何信息
    ERROR  = 1,   // 仅输出错误信息（数据比对失败等）— 默认
    INFO   = 2,   // 输出 INFO + ERROR 信息（包括事务摘要、统计等）
    DEBUG  = 3,   // 输出所有信息（包括每笔事务详情、WARN 等调试信息）
};

// ============================================================
// Logger 单例类
// ============================================================
class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    // ---- 级别控制 ----
    void set_level(LogLevel level) { level_ = level; }
    LogLevel get_level() const { return level_; }

    // ---- 文件输出控制 ----
    void enable_file_output(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (log_file_.is_open()) log_file_.close();
        log_file_.open(filename, std::ios::out | std::ios::trunc);
        file_enabled_ = log_file_.is_open();
    }

    void disable_file_output() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (log_file_.is_open()) log_file_.close();
        file_enabled_ = false;
    }

    // ---- 日志输出接口 ----

    // ERROR: 数据比对失败等严重错误
    void error(const std::string& msg) {
        if (level_ < LogLevel::ERROR) return;
        std::lock_guard<std::mutex> lock(mtx_);
        std::cerr << msg;
        write_to_file(msg);
    }

    // INFO: 常规信息（统计摘要等）
    void info(const std::string& msg) {
        if (level_ < LogLevel::INFO) return;
        std::lock_guard<std::mutex> lock(mtx_);
        std::cout << msg;
        write_to_file(msg);
    }

    // DEBUG: 调试信息（WARN、每笔事务详情等）
    void debug(const std::string& msg) {
        if (level_ < LogLevel::DEBUG) return;
        std::lock_guard<std::mutex> lock(mtx_);
        std::cout << msg;
        write_to_file(msg);
    }

    // ALWAYS: 不受级别控制，始终输出（用于 final_report）
    void always(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::cout << msg;
        write_to_file(msg);
    }

    // FATAL: 不受级别控制，输出到 stderr
    void fatal(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::cerr << msg;
        write_to_file(msg);
    }

    ~Logger() {
        if (log_file_.is_open()) log_file_.close();
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void write_to_file(const std::string& msg) {
        if (file_enabled_ && log_file_.is_open()) {
            log_file_ << msg;
            log_file_.flush();
        }
    }

    LogLevel      level_        = LogLevel::ERROR;
    std::ofstream log_file_;
    bool          file_enabled_ = false;
    std::mutex    mtx_;
};

// ============================================================
// 便捷宏：避免在低级别时构造字符串的开销
// ============================================================
#define LOG_ERROR(msg) \
    do { \
        if (Logger::instance().get_level() >= LogLevel::ERROR) { \
            std::ostringstream _oss; _oss << msg; \
            Logger::instance().error(_oss.str()); \
        } \
    } while(0)

#define LOG_INFO(msg) \
    do { \
        if (Logger::instance().get_level() >= LogLevel::INFO) { \
            std::ostringstream _oss; _oss << msg; \
            Logger::instance().info(_oss.str()); \
        } \
    } while(0)

#define LOG_DEBUG(msg) \
    do { \
        if (Logger::instance().get_level() >= LogLevel::DEBUG) { \
            std::ostringstream _oss; _oss << msg; \
            Logger::instance().debug(_oss.str()); \
        } \
    } while(0)

#define LOG_ALWAYS(msg) \
    do { \
        std::ostringstream _oss; _oss << msg; \
        Logger::instance().always(_oss.str()); \
    } while(0)

#define LOG_FATAL(msg) \
    do { \
        std::ostringstream _oss; _oss << msg; \
        Logger::instance().fatal(_oss.str()); \
    } while(0)
