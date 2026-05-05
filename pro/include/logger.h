#pragma once

#include <string>

/**
 * @brief 日志级别（数值越大表示越严重）
 *
 * 论文写作建议：日志用于“实验可复现性”与“协议交互可观测性”。
 * 本项目统一输出格式：`[time][LEVEL] message`。
 */
enum class LogLevel {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3
};

/**
 * @brief 日志初始化配置
 *
 * - level：最小输出级别
 * - file_path：非空则同时写文件（追加模式），用于保留实验过程与集成测试证据。
 */
struct LoggerConfig {
  LogLevel level = LogLevel::Info;  // 最低输出级别
  std::string file_path;            // 日志文件路径，空则仅输出到控制台
};

/**
 * @brief 初始化日志（建议在程序启动时调用一次）
 *
 * @note 该实现使用全局配置 + mutex 做线程安全输出，足以覆盖当前多线程场景（如 console 模式）。
 */
void InitLogger(const LoggerConfig& cfg);

/// 按级别输出日志（内部会做 level 过滤）
void LogDebug(const std::string& msg);
void LogInfo(const std::string& msg);
void LogWarn(const std::string& msg);
void LogError(const std::string& msg);

/**
 * @brief 将字符串解析为日志级别（debug/info/warn/error）
 *
 * @return 无法识别时默认返回 Info（兼容脚本/配置的容错策略）
 */
LogLevel ParseLogLevel(const std::string& level);
