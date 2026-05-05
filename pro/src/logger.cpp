#include "logger.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace {
// 全局日志配置与输出句柄（简化依赖：不引入第三方 logging 库）
LoggerConfig g_config;
std::mutex g_log_mutex;
std::ofstream g_log_file;

/// 将日志级别枚举转换为固定字符串（用于日志行格式）
const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
  }
  return "INFO";
}

/// 判断当前级别是否需要输出（level 越大越严重，过滤逻辑为：level >= config.level）
bool ShouldLog(LogLevel level) {
  return static_cast<int>(level) >= static_cast<int>(g_config.level);
}

/// 获取当前时间字符串（用于日志时间戳；格式 `%Y-%m-%d %H:%M:%S`）
std::string NowTimeString() {
  using clock = std::chrono::system_clock;
  auto now = clock::now();
  std::time_t t = clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

/**
 * @brief 输出一条日志到控制台与文件
 *
 * 线程安全策略：对整条日志行加锁，保证多线程情况下不交织输出。
 * 文件写入采用 flush，牺牲一点性能换取“测试失败时也能尽量保留最后日志”。
 */
void LogInternal(LogLevel level, const std::string& msg) {
  if (!ShouldLog(level)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_log_mutex);
  const std::string line = "[" + NowTimeString() + "]" +
                           "[" + LevelName(level) + "] " + msg;
  std::cerr << line << "\n";
  if (g_log_file.is_open()) {
    g_log_file << line << "\n";
    g_log_file.flush();
  }
}
}  // namespace

void InitLogger(const LoggerConfig& cfg) {
  g_config = cfg;
  // 打开日志文件（追加模式）：便于多次运行脚本累计记录。
  if (!g_config.file_path.empty()) {
    g_log_file.open(g_config.file_path, std::ios::out | std::ios::app);
  }
}

void LogDebug(const std::string& msg) { LogInternal(LogLevel::Debug, msg); }
void LogInfo(const std::string& msg) { LogInternal(LogLevel::Info, msg); }
void LogWarn(const std::string& msg) { LogInternal(LogLevel::Warn, msg); }
void LogError(const std::string& msg) { LogInternal(LogLevel::Error, msg); }

LogLevel ParseLogLevel(const std::string& level) {
  if (level == "debug") {
    return LogLevel::Debug;
  }
  if (level == "info") {
    return LogLevel::Info;
  }
  if (level == "warn") {
    return LogLevel::Warn;
  }
  if (level == "error") {
    return LogLevel::Error;
  }
  return LogLevel::Info;
}
