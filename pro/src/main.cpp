// 程序入口：负责读取配置、初始化日志、打印启动信息
// MVP1 支持文件分片与校验（命令行触发）

#include "config.h"
#include "chunk_store.h"
#include "hex_utils.h"
#include "logger.h"
#include "routing_table.h"
#include "secure_transport.h"
#include "sha256.h"
#include "tcp_transport.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <arpa/inet.h>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
    // 解析命令行中的 --config 参数
    std::string ParseConfigPath(int argc, char **argv) {
        std::string config_path = "configs/app.yaml";
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--config" && i + 1 < argc) {
                config_path = argv[i + 1];
                ++i;
            }
        }
        return config_path;
    }

    // 解析命令行中的指定参数值（如 --input /path/to/file）
    std::string ParseArgValue(int argc, char **argv, const std::string &key) {
        for (int i = 1; i < argc; ++i) {
            if (key == argv[i] && i + 1 < argc) {
                return argv[i + 1];
            }
        }
        return "";
    }

    // 判断是否包含某个命令行开关（如 --secure）
    bool HasFlag(int argc, char **argv, const std::string &key) {
        for (int i = 1; i < argc; ++i) {
            if (key == argv[i]) {
                return true;
            }
        }
        return false;
    }

    // 去除首尾空白字符
    std::string TrimLocal(const std::string &s) {
        const char *whitespace = " \t\r\n";
        const auto start = s.find_first_not_of(whitespace);
        if (start == std::string::npos) {
            return "";
        }
        const auto end = s.find_last_not_of(whitespace);
        return s.substr(start, end - start + 1);
    }

    // URL 解码（仅处理 %xx 与 + 空格）
    std::string UrlDecode(const std::string &input) {
        std::string out;
        out.reserve(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            char c = input[i];
            if (c == '+') {
                out.push_back(' ');
                continue;
            }
            if (c == '%' && i + 2 < input.size()) {
                const std::string hex = input.substr(i + 1, 2);
                char *end = nullptr;
                long v = std::strtol(hex.c_str(), &end, 16);
                if (end && *end == '\0') {
                    out.push_back(static_cast<char>(v));
                    i += 2;
                    continue;
                }
            }
            out.push_back(c);
        }
        return out;
    }

    std::map<std::string, std::string> ParseQuery(const std::string &query) {
        std::map<std::string, std::string> out;
        std::size_t start = 0;
        while (start < query.size()) {
            const auto amp = query.find('&', start);
            const auto item = query.substr(start, amp == std::string::npos ? query.size() - start : amp - start);
            const auto eq = item.find('=');
            if (eq != std::string::npos) {
                const std::string k = UrlDecode(item.substr(0, eq));
                const std::string v = UrlDecode(item.substr(eq + 1));
                out[k] = v;
            }
            if (amp == std::string::npos) {
                break;
            }
            start = amp + 1;
        }
        return out;
    }

    // 解析逗号分隔列表
    std::vector<std::string> SplitList(const std::string &value) {
        std::vector<std::string> out;
        std::string cur;
        std::istringstream iss(value);
        while (std::getline(iss, cur, ',')) {
            const std::string trimmed = TrimLocal(cur);
            if (!trimmed.empty()) {
                out.push_back(trimmed);
            }
        }
        return out;
    }

    // 按 TAB 分割
    std::vector<std::string> SplitTabs(const std::string &value) {
        std::vector<std::string> out;
        std::string cur;
        std::istringstream iss(value);
        while (std::getline(iss, cur, '\t')) {
            out.push_back(cur);
        }
        return out;
    }

    // 简单 JSON 转义
    std::string EscapeJson(const std::string &input) {
        std::string out;
        out.reserve(input.size());
        for (char c: input) {
            switch (c) {
                case '\"': out += "\\\"";
                    break;
                case '\\': out += "\\\\";
                    break;
                case '\n': out += "\\n";
                    break;
                case '\r': out += "\\r";
                    break;
                case '\t': out += "\\t";
                    break;
                default: out += c;
                    break;
            }
        }
        return out;
    }

    int ParseIntOrDefault(const std::string &value, int default_value);

    bool UploadAndReplicate(const std::string &input_path,
                            const std::string &output_dir,
                            const std::string &index_path,
                            const std::string &manifest_out,
                            const AppConfig &cfg,
                            int replica_count,
                            RoutingTable *table,
                            std::map<std::string, std::vector<std::string> > *dht_store);

    bool DownloadByManifest(const std::string &manifest_in,
                            const std::string &out_file,
                            const std::string &peer,
                            const AppConfig &cfg,
                            const std::string &strategy,
                            bool secure_mode,
                            RoutingTable *table);

    struct DownloadStatRow;
    struct UploadMetaRow;
    struct UploadReplicaRow;

    bool LoadDownloadStats(const std::string &path, std::vector<DownloadStatRow> *out);

    bool LoadUploadMeta(const std::string &path, std::vector<UploadMetaRow> *out);

    bool LoadUploadReplica(const std::string &path, std::vector<UploadReplicaRow> *out);

    std::string JoinList(const std::vector<std::string> &items) {
        std::string out;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i > 0) {
                out += ",";
            }
            out += items[i];
        }
        return out;
    }

    // 解析 host:port 形式的地址
    bool ParseHostPort(const std::string &value, std::string *host, int *port) {
        auto pos = value.find(':');
        if (pos == std::string::npos) {
            return false;
        }
        *host = value.substr(0, pos);
        *port = std::stoi(value.substr(pos + 1));
        return !host->empty() && *port > 0;
    }

    // 打印当前加载的配置，便于启动时检查
    void PrintConfig(const AppConfig &cfg) {
        LogInfo("config.node_id=" + cfg.node_id);
        LogInfo("config.listen_port=" + std::to_string(cfg.listen_port));
        LogInfo("config.dht_k=" + std::to_string(cfg.dht_k));
        LogInfo("config.max_nodes=" + std::to_string(cfg.max_nodes));
        LogInfo("config.seed_nodes=" + std::to_string(cfg.seed_nodes.size()));
        for (const auto &node: cfg.seed_nodes) {
            LogInfo("seed_node=" + node);
        }
        LogInfo("config.self_addr=" + cfg.self_addr);
        LogInfo("config.chunk_size_mb=" + std::to_string(cfg.chunk_size_mb));
        LogInfo("config.chunks_dir=" + cfg.chunks_dir);
        LogInfo("config.chunk_index_file=" + cfg.chunk_index_file);
        LogInfo("config.routing_capacity=" + std::to_string(cfg.routing_capacity));
        LogInfo("config.aes_key_hex_len=" + std::to_string(cfg.aes_key_hex.size()));
        LogInfo("config.hmac_key_hex_len=" + std::to_string(cfg.hmac_key_hex.size()));
        LogInfo("config.download_strategy=" + cfg.download_strategy);
        LogInfo("config.download_stats_file=" + cfg.download_stats_file);
        LogInfo("config.upload_meta_file=" + cfg.upload_meta_file);
        LogInfo("config.upload_replica_file=" + cfg.upload_replica_file);
        LogInfo("config.http_port=" + std::to_string(cfg.http_port));
        LogInfo("config.log_level=" + cfg.log_level);
        LogInfo("config.log_file=" + cfg.log_file);
    }

    // 生成当前时间字符串，作为启动日志的一部分
    std::string CurrentTimeString() {
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

    // 追加记录一次下载尝试的统计信息（用于成功率/延迟打分）
    bool AppendDownloadStat(const std::string &stats_path,
                            const std::string &chunk_id,
                            const std::string &owner,
                            bool success,
                            long latency_ms,
                            const std::string &strategy) {
        if (stats_path.empty()) {
            return true;
        }
        std::filesystem::path p(stats_path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        std::ofstream out(stats_path, std::ios::out | std::ios::app);
        if (!out.is_open()) {
            LogWarn("stats: cannot open " + stats_path);
            return false;
        }
        out << CurrentTimeString() << "\t"
                << chunk_id << "\t"
                << owner << "\t"
                << (success ? "1" : "0") << "\t"
                << latency_ms << "\t"
                << strategy << "\n";
        return true;
    }

    // 追加记录一次上传文件的元数据（文件名/大小/清单路径）
    bool AppendUploadMeta(const std::string &meta_path,
                          const std::string &file_name,
                          std::uintmax_t file_size,
                          const std::string &uploader,
                          const std::string &manifest_path) {
        if (meta_path.empty()) {
            return true;
        }
        std::filesystem::path p(meta_path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        std::ofstream out(meta_path, std::ios::out | std::ios::app);
        if (!out.is_open()) {
            LogWarn("upload meta: cannot open " + meta_path);
            return false;
        }
        out << CurrentTimeString() << "\t"
                << file_name << "\t"
                << file_size << "\t"
                << uploader << "\t"
                << manifest_path << "\n";
        return true;
    }

    // 追加记录一次分片副本分发结果
    bool AppendReplicaResult(const std::string &replica_path,
                             const std::string &chunk_id,
                             const std::string &target,
                             bool success) {
        if (replica_path.empty()) {
            return true;
        }
        std::filesystem::path p(replica_path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        std::ofstream out(replica_path, std::ios::out | std::ios::app);
        if (!out.is_open()) {
            LogWarn("upload replica: cannot open " + replica_path);
            return false;
        }
        out << CurrentTimeString() << "\t"
                << chunk_id << "\t"
                << target << "\t"
                << (success ? "1" : "0") << "\n";
        return true;
    }

    struct DownloadStatRow {
        std::string time;
        std::string chunk_id;
        std::string owner;
        bool success = false;
        long latency_ms = 0;
        std::string strategy;
    };

    struct UploadMetaRow {
        std::string time;
        std::string file_name;
        std::string file_size;
        std::string uploader;
        std::string manifest;
    };

    struct UploadReplicaRow {
        std::string time;
        std::string chunk_id;
        std::string target;
        bool success = false;
    };

    std::string BuildDownloadStatsJson(const std::vector<DownloadStatRow> &rows,
                                       const std::string &owner_filter,
                                       int limit) {
        struct Agg {
            int total = 0;
            int success = 0;
            long latency_sum = 0;
        };
        std::map<std::string, Agg> agg;
        std::map<std::string, std::string> last_time;
        for (const auto &r: rows) {
            if (!owner_filter.empty() && r.owner != owner_filter) {
                continue;
            }
            auto &entry = agg[r.owner];
            entry.total += 1;
            entry.success += (r.success ? 1 : 0);
            entry.latency_sum += r.latency_ms;
            last_time[r.owner] = r.time;
        }

        std::ostringstream oss;
        oss << "{";
        oss << "\"type\":\"download\",";
        oss << "\"total_rows\":" << rows.size() << ",";
        oss << "\"items\":[";
        bool first = true;
        int count = 0;
        for (const auto &r: rows) {
            if (!owner_filter.empty() && r.owner != owner_filter) {
                continue;
            }
            if (count >= limit) {
                break;
            }
            if (!first) {
                oss << ",";
            }
            first = false;
            oss << "{";
            oss << "\"time\":\"" << EscapeJson(r.time) << "\",";
            oss << "\"chunk_id\":\"" << EscapeJson(r.chunk_id) << "\",";
            oss << "\"owner\":\"" << EscapeJson(r.owner) << "\",";
            oss << "\"success\":" << (r.success ? 1 : 0) << ",";
            oss << "\"latency_ms\":" << r.latency_ms << ",";
            oss << "\"strategy\":\"" << EscapeJson(r.strategy) << "\"";
            oss << "}";
            count++;
        }
        oss << "],";
        oss << "\"summary\":[";
        bool sfirst = true;
        for (const auto &it: agg) {
            if (!sfirst) {
                oss << ",";
            }
            sfirst = false;
            const int total = it.second.total;
            const int ok = it.second.success;
            const long total_latency = it.second.latency_sum;
            oss << "{";
            oss << "\"owner\":\"" << EscapeJson(it.first) << "\",";
            oss << "\"total\":" << total << ",";
            oss << "\"success\":" << ok << ",";
            oss << "\"latency_ms_sum\":" << total_latency << ",";
            oss << "\"last_time\":\"" << EscapeJson(last_time[it.first]) << "\"";
            oss << "}";
        }
        oss << "]";
        oss << "}";
        return oss.str();
    }

    std::string BuildUploadMetaJson(const std::vector<UploadMetaRow> &rows, int limit) {
        std::ostringstream oss;
        oss << "{";
        oss << "\"type\":\"upload_meta\",";
        oss << "\"total_rows\":" << rows.size() << ",";
        oss << "\"items\":[";
        bool first = true;
        int count = 0;
        for (const auto &r: rows) {
            if (count >= limit) {
                break;
            }
            if (!first) {
                oss << ",";
            }
            first = false;
            oss << "{";
            oss << "\"time\":\"" << EscapeJson(r.time) << "\",";
            oss << "\"file_name\":\"" << EscapeJson(r.file_name) << "\",";
            oss << "\"file_size\":\"" << EscapeJson(r.file_size) << "\",";
            oss << "\"uploader\":\"" << EscapeJson(r.uploader) << "\",";
            oss << "\"manifest\":\"" << EscapeJson(r.manifest) << "\"";
            oss << "}";
            count++;
        }
        oss << "]";
        oss << "}";
        return oss.str();
    }

    std::string BuildUploadReplicaJson(const std::vector<UploadReplicaRow> &rows,
                                       const std::string &owner_filter,
                                       int limit) {
        std::ostringstream oss;
        oss << "{";
        oss << "\"type\":\"upload_replica\",";
        oss << "\"total_rows\":" << rows.size() << ",";
        oss << "\"items\":[";
        bool first = true;
        int count = 0;
        for (const auto &r: rows) {
            if (!owner_filter.empty() && r.target != owner_filter) {
                continue;
            }
            if (count >= limit) {
                break;
            }
            if (!first) {
                oss << ",";
            }
            first = false;
            oss << "{";
            oss << "\"time\":\"" << EscapeJson(r.time) << "\",";
            oss << "\"chunk_id\":\"" << EscapeJson(r.chunk_id) << "\",";
            oss << "\"target\":\"" << EscapeJson(r.target) << "\",";
            oss << "\"success\":" << (r.success ? 1 : 0);
            oss << "}";
            count++;
        }
        oss << "]";
        oss << "}";
        return oss.str();
    }

    void MergeOwnersList(const std::vector<std::string> &src,
                         std::vector<std::string> *dst) {
        if (!dst) {
            return;
        }
        for (const auto &s: src) {
            bool exists = false;
            for (const auto &d: *dst) {
                if (d == s) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                dst->push_back(s);
            }
        }
    }

    void UpsertDhtValue(std::map<std::string, std::vector<std::string> > *store,
                        const std::string &key,
                        const std::vector<std::string> &owners) {
        if (!store || key.empty()) {
            return;
        }
        auto &entry = (*store)[key];
        MergeOwnersList(owners, &entry);
    }

    std::vector<std::string> ExtractListFromResp(const std::string &prefix,
                                                 const std::vector<std::string> &resps) {
        if (resps.empty()) {
            return {};
        }
        if (resps[0].rfind(prefix, 0) != 0) {
            return {};
        }
        const std::string list = TrimLocal(resps[0].substr(prefix.size()));
        return SplitList(list);
    }

    //在 Kademlia 分布式网络中，通过「迭代询问邻居节点」的方式，全网搜索离目标 (target) 最近的 k 个节点
    //比如说用于先找到节点然后发消息
    std::vector<std::string> IterativeFindNodes(const std::string &target, // 目标：要找的 节点ID/文件哈希
                                                RoutingTable *table, // 本地路由表（存我认识的所有节点）
                                                const std::vector<std::string> &seeds, // 种子节点：网络入口（刚入网不认识人，用它起步）
                                                std::size_t k, // 要找多少个最近节点（k=20
                                                int max_queries) {
        // 最大查询次数：防止无限循环
        std::vector<std::string> shortlist;
        // 第一步：从本地路由表，先拿出离目标最近的k个节点
        if (table) {
            shortlist = table->TopKClosestKeys(target, k);
        }
        // 如果本地一个节点都没有，就用种子节点当起步
        if (shortlist.empty()) {
            shortlist = seeds;
        }
        //防重复查询：记录已访问的节点
        std::set<std::string> visited; // 存已经问过的节点
        int queries = 0; // 记录发了多少次请求
        for (std::size_t i = 0; i < shortlist.size() && queries < max_queries; ++i) {
            const std::string node = shortlist[i];
            if (visited.count(node)) {
                continue;
            }
            visited.insert(node);
            std::string host;
            int port = 0;
            if (!ParseHostPort(node, &host, &port)) {
                continue;
            }
            std::vector<std::string> resps;
            RunTcpClientSession(host, port, {"FIND_NODE " + target}, &resps);
            queries++;
            const auto nodes = ExtractListFromResp("FIND_NODE_RESP", resps);
            for (const auto &n: nodes) {
                if (table) {
                    table->AddNode(n);
                }
                if (std::find(shortlist.begin(), shortlist.end(), n) == shortlist.end()) {
                    shortlist.push_back(n);
                }
            }
        }
        if (table) {
            return table->TopKClosestKeys(target, k);
        }
        return shortlist;
    }

    std::vector<std::string> IterativeFindValue(const std::string &key, // 入参：要查找的 数据ID（比如你的 chunk_id）
                                                RoutingTable *table, // 入参：本地路由表（本机认识的所有网络节点）
                                                const std::vector<std::string> &seeds, // 入参：种子节点（网络入口，刚入网时用）
                                                std::size_t k, // 入参：每次找「最近的k个节点」去询问
                                                int max_queries) {
        // 入参：最大查询次数（防止无限循环查网络）

        // ====================== 1. 初始化候选节点列表 ======================
        // shortlist：候选列表 → 存储「即将去询问的节点」
        std::vector<std::string> shortlist;

        // 如果本地有路由表 → 先拿「离key最近的k个节点」作为初始候选
        // 优先问近的，效率最高（Kademlia 核心思想：近的节点更可能存数据）
        if (table) {
            shortlist = table->TopKClosestKeys(key, k);
        }

        // 如果本地一个节点都没有 → 用种子节点当起步候选
        if (shortlist.empty()) {
            shortlist = seeds;
        }

        // ====================== 2. 防重复查询 ======================
        // visited：记录已经询问过的节点，避免重复问同一个节点
        std::set<std::string> visited;
        // queries：记录已经发了多少次网络请求，达到max_queries就停止
        int queries = 0;

        // ====================== 3. 核心迭代循环：遍历候选节点去询问 ======================
        // 循环条件：候选列表没遍历完 + 没超过最大查询次数
        for (std::size_t i = 0; i < shortlist.size() && queries < max_queries; ++i) {
            // 当前要询问的节点
            const std::string node = shortlist[i];
            // 如果这个节点已经问过了 → 跳过
            if (visited.count(node)) {
                continue;
            }

            // 标记为已访问
            visited.insert(node);

            // 解析节点地址：把 "ip:port" 拆成 host 和 port
            std::string host;
            int port = 0;
            if (!ParseHostPort(node, &host, &port)) {
                continue;
            }

            // ====================== 4. 发送网络查询指令 ======================
            // 关键指令：FIND_VALUE + key → 问节点：你有这个数据吗？谁存了它？
            std::vector<std::string> resps;
            RunTcpClientSession(host, port, {"FIND_VALUE " + key}, &resps);
            queries++;

            // ====================== 5. 处理响应：找到数据 → 直接返回！ ======================
            // 从响应中提取「数据存储节点列表」（响应头：FIND_VALUE_RESP）
            auto values = ExtractListFromResp("FIND_VALUE_RESP", resps);

            // ✅ 核心特性：只要找到数据/存储节点，立刻终止所有查询，直接返回！
            if (!values.empty()) {
                //-----------------------------------------这个是跟那个find node不一样的地方，因为这个只要一份数据
                return values;
            }
            // ====================== 6. 没找到数据 → 继续扩展节点列表 ======================
            // 节点回复：我不知道，但我给你推荐其他节点（响应头：FIND_NODE_RESP）
            const auto nodes = ExtractListFromResp("FIND_NODE_RESP", resps);

            // 遍历推荐的新节点
            for (const auto &n: nodes) {
                // 把新节点加入本地路由表（更新自己的人脉）
                if (table) {
                    table->AddNode(n);
                }
                // 如果新节点不在候选列表里 → 加入候选，后续继续问
                if (std::find(shortlist.begin(), shortlist.end(), n) == shortlist.end()) {
                    shortlist.push_back(n);
                }
            }
        }
        return {};
    }

    //：将 chunk 的元数据（chunk_id → owners 映射）存储到 P2P 网络的 DHT 中。要做网络传输，其实就是调用一下RunTcpClientSession
    void StoreValueToDht(const std::string &key, // ← chunk_id（要存储的键）
                         const std::vector<std::string> &owners, // ← 拥有该 chunk 的节点列表（值）
                         RoutingTable *table, // ← 本地路由表
                         const std::vector<std::string> &seeds, // ← 种子节点列表（入口点）
                         std::size_t k) {
        // ← DHT 的 K 值（副本数/查询节点数）

        if (key.empty() || owners.empty() || !table) {
            return;
        } //在 Kademlia 分布式网络中，通过「迭代询问邻居节点」的方式，全网搜索离目标 (target) 最近的 k 个节点
        const auto targets = IterativeFindNodes(key, table, seeds, k, 8);
        for (const auto &node: targets) {
            std::string host;
            int port = 0;
            if (!ParseHostPort(node, &host, &port)) {
                continue;
            }
            const std::string req = "STORE " + key + " " + JoinList(owners);
            std::vector<std::string> resps;
            //
            RunTcpClientSession(host, port, {req}, &resps);
        }
    }

    std::string MakeHttpResponse(int code, const std::string &body) {
        const std::string status = (code == 200) ? "OK" : (code == 404) ? "Not Found" : "Bad Request";
        std::ostringstream oss;
        oss << "HTTP/1.1 " << code << " " << status << "\r\n";
        oss << "Content-Type: application/json\r\n";
        oss << "Access-Control-Allow-Origin: *\r\n";
        oss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
        oss << "Access-Control-Allow-Headers: Content-Type\r\n";
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "Connection: close\r\n";
        oss << "\r\n";
        oss << body;
        return oss.str();
    }

    void RunHttpServer(const AppConfig &cfg,
                       RoutingTable *table,
                       std::map<std::string, std::vector<std::string> > *dht_store) {
        // 获取配置的HTTP端口
        const int port = cfg.http_port;
        // 创建TCP套接字（AF_INET=IPv4，SOCK_STREAM=TCP）
        int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            LogError("http: socket create failed");
            return;
        }

        // 设置端口复用：重启程序不会提示端口被占用
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));


        //--------------------------------------------------绑定 IP + 端口 / 监听连接
        // 定义服务端地址结构体
        sockaddr_in addr{};
        addr.sin_family = AF_INET; // IPv4
        addr.sin_port = htons(static_cast<uint16_t>(port)); // 端口（转网络字节序）
        addr.sin_addr.s_addr = INADDR_ANY; // 监听所有网卡（0.0.0.0）

        // 绑定socket到端口
        if (bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            LogError("http: bind failed");
            close(server_fd);
            return;
        }
        // 开始监听，最大等待队列16个连接
        if (listen(server_fd, 16) != 0) {
            LogError("http: listen failed");
            close(server_fd);
            return;
        }
        LogInfo("http: server listening on 0.0.0.0:" + std::to_string(port));

        // ---------------------------------------------------------------------------------------死循环：持续处理客户端连接
        for (;;) {
            sockaddr_in client{}; // 客户端地址
            socklen_t len = sizeof(client);
            // 接受客户端连接（阻塞等待，直到有请求进来）
            int client_fd = accept(server_fd, reinterpret_cast<sockaddr *>(&client), &len);
            if (client_fd < 0) {
                continue; // 连接失败，跳过
            }
            //--------------------------------------------------------------------------------------模块4：接受http请求

            // 接收缓冲区：8KB（足够存储HTTP请求头）
            std::string buffer;
            buffer.resize(8192);
            // 读取客户端发送的HTTP数据
            ssize_t n = recv(client_fd, &buffer[0], buffer.size(), 0);
            if (n <= 0) {
                close(client_fd);
                continue;
            }
            // 调整缓冲区大小为实际接收的数据长度
            buffer.resize(static_cast<std::size_t>(n));

            // 找到HTTP头结束标志：\r\n\r\n（请求头和请求体分隔符）
            const auto header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                // 请求格式错误，返回400
                const std::string body = "{\"error\":\"bad request\"}";
                const std::string resp = MakeHttpResponse(400, body);
                send(client_fd, resp.data(), resp.size(), 0);
                close(client_fd);
                continue;
            }

            // 解析请求行：GET /status HTTP/1.1
            std::istringstream iss(buffer.substr(0, header_end));
            std::string method; // 请求方法：GET/POST/OPTIONS
            std::string target; // 请求目标：/status?xxx=xxx
            std::string version; // HTTP版本
            iss >> method >> target >> version;


            // 拆分路径和查询参数：/download?manifest=xx → path=/download，query=manifest=xx
            std::string path = target;
            std::string query;
            const auto qpos = target.find('?');
            if (qpos != std::string::npos) {
                path = target.substr(0, qpos);
                query = target.substr(qpos + 1);
            }

            // 解析查询参数为键值对（?a=1&b=2 → {a:1,b:2}）
            const auto params = ParseQuery(query);

            std::string body = "{\"ok\":true}";
            int code = 200;
            if (method == "OPTIONS") {
                const std::string resp = MakeHttpResponse(200, "{}");
                send(client_fd, resp.data(), resp.size(), 0);
                close(client_fd);
                continue;
            }
            if (method == "GET" && path == "/status") {
                // 返回JSON：节点ID、自身地址、当前时间
                std::ostringstream oss;
                oss << "{";
                oss << "\"node_id\":\"" << EscapeJson(cfg.node_id) << "\",";
                oss << "\"self_addr\":\"" << EscapeJson(cfg.self_addr) << "\",";
                oss << "\"time\":\"" << EscapeJson(CurrentTimeString()) << "\"";
                oss << "}";
                body = oss.str();
            } else if (method == "POST" && path == "/upload") {
                // 获取参数：文件路径、清单输出路径、副本数
                const std::string file = params.count("file") ? params.at("file") : "";
                const std::string manifest = params.count("manifest_out") ? params.at("manifest_out") : "";
                const int replica = params.count("replica") ? ParseIntOrDefault(params.at("replica"), 0) : 0;
                // 参数校验
                if (file.empty()) {
                    code = 400;
                    body = "{\"error\":\"missing file\"}";
                } else {
                    // 核心：上传文件→分片→生成manifest→分布式存储副本
                    const bool ok = UploadAndReplicate(file, cfg.chunks_dir, cfg.chunk_index_file,
                                                       manifest, cfg, replica, table, dht_store);
                    body = ok ? "{\"ok\":true}" : "{\"ok\":false}";
                }
            } else if (method == "POST" && path == "/download") {
                // 获取参数：清单文件、输出文件、对端节点、下载策略、加密模式
                const std::string manifest = params.count("manifest") ? params.at("manifest") : "";
                const std::string out_file = params.count("out") ? params.at("out") : "";
                const std::string peer = params.count("peer") ? params.at("peer") : "";
                const std::string strategy = params.count("strategy") ? params.at("strategy") : cfg.download_strategy;
                const bool secure = params.count("secure") ? (params.at("secure") == "1") : false;
                if (manifest.empty() || out_file.empty() || peer.empty()) {
                    code = 400;
                    body = "{\"error\":\"missing manifest/out/peer\"}";
                } else {
                    // 核心：调用你之前的下载逻辑→批量下载chunk→拼接成完整文件
                    const bool ok = DownloadByManifest(manifest, out_file, peer, cfg, strategy,
                                                       secure, table);
                    body = ok ? "{\"ok\":true}" : "{\"ok\":false}";
                }
            } else if (method == "GET" && path == "/stats") {
                const std::string type = params.count("type") ? params.at("type") : "download";
                const std::string owner = params.count("owner") ? params.at("owner") : "";
                const int limit = params.count("limit") ? ParseIntOrDefault(params.at("limit"), 50) : 50;
                if (type == "download") {
                    std::vector<DownloadStatRow> rows;
                    LoadDownloadStats(cfg.download_stats_file, &rows);
                    body = BuildDownloadStatsJson(rows, owner, limit);
                } else if (type == "upload_meta") {
                    std::vector<UploadMetaRow> rows;
                    LoadUploadMeta(cfg.upload_meta_file, &rows);
                    body = BuildUploadMetaJson(rows, limit);
                } else if (type == "upload_replica") {
                    std::vector<UploadReplicaRow> rows;
                    LoadUploadReplica(cfg.upload_replica_file, &rows);
                    body = BuildUploadReplicaJson(rows, owner, limit);
                } else {
                    code = 400;
                    body = "{\"error\":\"invalid type\"}";
                }
            } else {
                code = 404;
                body = "{\"error\":\"not found\"}";
            }

            const std::string resp = MakeHttpResponse(code, body);
            send(client_fd, resp.data(), resp.size(), 0);
            close(client_fd);
        }
    }

    bool LoadDownloadStats(const std::string &path, std::vector<DownloadStatRow> *out) {
        if (!out) {
            return false;
        }
        out->clear();
        std::ifstream in(path);
        if (!in.is_open()) {
            LogWarn("stats: cannot open " + path);
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            const auto parts = SplitTabs(line);
            if (parts.size() < 6) {
                continue;
            }
            DownloadStatRow row;
            row.time = parts[0];
            row.chunk_id = parts[1];
            row.owner = parts[2];
            row.success = (parts[3] == "1");
            try {
                row.latency_ms = std::stol(parts[4]);
            } catch (...) {
                row.latency_ms = 0;
            }
            row.strategy = parts[5];
            out->push_back(row);
        }
        return true;
    }

    bool LoadUploadMeta(const std::string &path, std::vector<UploadMetaRow> *out) {
        if (!out) {
            return false;
        }
        out->clear();
        std::ifstream in(path);
        if (!in.is_open()) {
            LogWarn("upload meta: cannot open " + path);
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            const auto parts = SplitTabs(line);
            if (parts.size() < 5) {
                continue;
            }
            UploadMetaRow row;
            row.time = parts[0];
            row.file_name = parts[1];
            row.file_size = parts[2];
            row.uploader = parts[3];
            row.manifest = parts[4];
            out->push_back(row);
        }
        return true;
    }

    bool LoadUploadReplica(const std::string &path, std::vector<UploadReplicaRow> *out) {
        if (!out) {
            return false;
        }
        out->clear();
        std::ifstream in(path);
        if (!in.is_open()) {
            LogWarn("upload replica: cannot open " + path);
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            const auto parts = SplitTabs(line);
            if (parts.size() < 4) {
                continue;
            }
            UploadReplicaRow row;
            row.time = parts[0];
            row.chunk_id = parts[1];
            row.target = parts[2];
            row.success = (parts[3] == "1");
            out->push_back(row);
        }
        return true;
    }

    bool LoadCryptoKeys(const AppConfig &cfg, // 输入：应用配置对象（存了密钥的字符串）
                        std::vector<uint8_t> *aes_key, // 输出：存放AES二进制密钥的指针
                        std::vector<uint8_t> *hmac_key) {
        // 输出：存放HMAC二进制密钥的指针
        if (!aes_key || !hmac_key) {
            return false;
        }
        if (!DecodeHex(cfg.aes_key_hex, aes_key) || aes_key->size() != 16) {
            LogError("mvp4: invalid aes_key_hex (expect 16 bytes hex)");
            return false;
        }
        if (!DecodeHex(cfg.hmac_key_hex, hmac_key) || hmac_key->size() != 16) {
            LogError("mvp4: invalid hmac_key_hex (expect 16 bytes hex)");
            return false;
        }
        return true;
    }

    bool FindChunkOwners(const std::vector<ChunkInfo> &chunks,
                         const std::string &chunk_id,
                         std::vector<std::string> *owners) {
        if (!owners) {
            return false;
        }
        owners->clear();
        for (const auto &info: chunks) {
            if (info.chunk_id == chunk_id) {
                *owners = info.owners;
                return true;
            }
        }
        return false;
    }

    int ParseIntOrDefault(const std::string &value, int default_value) {
        if (value.empty()) {
            return default_value;
        }
        try {
            return std::stoi(value);
        } catch (...) {
            return default_value;
        }
    }

    bool SendChunkReplica(const std::string &peer,
                          const ChunkInfo &info) {
        std::string host;
        int port = 0;
        if (!ParseHostPort(peer, &host, &port)) {
            LogWarn("mvp5: replica peer invalid: " + peer);
            return false;
        }
        std::ifstream in(info.path, std::ios::binary);
        if (!in.is_open()) {
            LogWarn("mvp5: replica read failed: " + info.path);
            return false;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        std::vector<std::string> owners = info.owners;
        bool exists = false;
        for (const auto &item: owners) {
            if (item == peer) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            owners.push_back(peer);
        }
        const std::string owners_csv = JoinList(owners);
        const std::string hex = EncodeHex(data);
        const std::string req =
                "PUT_CHUNK_HEX2 " + info.chunk_id + " " + owners_csv + " " + hex;
        std::vector<std::string> resps;
        if (!RunTcpClientSession(host, port, {req}, &resps)) {
            LogWarn("mvp5: replica send failed: " + peer);
            return false;
        }
        if (!resps.empty() && resps[0] == "PUT_OK") {
            LogInfo("mvp5: replica ok to " + peer + " chunk=" + info.chunk_id);
            return true;
        }
        LogWarn("mvp5: replica response not ok from " + peer);
        return false;
    }

    void AddOwnerToChunk(const std::string &owner, ChunkInfo *info) {
        if (!info || owner.empty()) {
            return;
        }
        for (const auto &existing: info->owners) {
            if (existing == owner) {
                return;
            }
        }
        info->owners.push_back(owner);
    }


    //这个就是判断那个是否roundrobin的那个方法
    void ApplyDownloadStrategy(const std::string &strategy,
                               std::vector<std::string> *owners) {
        if (!owners || owners->size() <= 1) {
            return;
        }
        if (strategy == "round_robin") {
            static std::size_t rr_index = 0;
            const std::size_t offset = rr_index % owners->size();
            std::rotate(owners->begin(), owners->begin() + offset, owners->end());
            rr_index++;
            return;
        }
        if (strategy == "random") {
            std::random_device rd;
            std::mt19937 rng(rd());
            std::shuffle(owners->begin(), owners->end(), rng);
            return;
        }
    }

    //实际执行下载
    bool DownloadChunkByOwners(const std::vector<std::string> &owners,
                               const std::string &chunk_id,
                               bool secure_mode,
                               const std::vector<uint8_t> &aes_key,
                               const std::vector<uint8_t> &hmac_key,
                               const std::string &self_addr,
                               const std::string &stats_file,
                               const std::string &strategy,
                               std::vector<uint8_t> *out_data) {
        if (!out_data) {
            return false;
        }
        // 遍历所有拥有该 chunk 的节点，逐个尝试下载
        for (const auto &owner: owners) {
            // 如果当前节点就是拥有者自己，跳过（不需要通过网络下载自己已有的数据）
            if (owner == self_addr) {
                continue;
            }
            // 记录下载开始时间（用于计算延迟）
            const auto start = std::chrono::steady_clock::now();

            // 声明变量用于存储解析后的主机地址和端口
            std::string h;
            int p = 0;

            // 解析节点地址字符串（如 "127.0.0.1:9002" → h="127.0.0.1", p=9002）
            // 如果解析失败，记录失败统计并跳过该节点
            if (!ParseHostPort(owner, &h, &p)) {
                AppendDownloadStat(stats_file, chunk_id, owner, false, 0, strategy);
                continue;
            }

            // 声明向量用于存储服务器的响应消息
            std::vector<std::string> resps;

            // 根据是否启用安全模式，选择不同的下载方式
            if (secure_mode) {
                // ===== 安全模式：加密传输 =====

                // 发送加密下载请求：GET_CHUNK_SEC <chunk_id>
                // 服务器会返回加密后的数据
                RunTcpClientSession(h, p, {"GET_CHUNK_SEC " + chunk_id}, &resps);

                // 检查响应是否存在且格式正确（应以 "CHUNK_SEC " 开头）
                if (!resps.empty() && resps[0].rfind("CHUNK_SEC ", 0) == 0) {
                    // 提取加密载荷部分（去掉 "CHUNK_SEC " 前缀）
                    const std::string payload =
                            resps[0].substr(std::string("CHUNK_SEC ").size());

                    // 声明向量用于存储解密后的明文数据
                    std::vector<uint8_t> plaintext;

                    // 尝试解密：使用 AES 解密 + HMAC 验签
                    // 如果解密或验签失败，记录警告和失败统计，继续尝试下一个节点
                    if (!DecryptFromSecureText(aes_key, hmac_key, payload, &plaintext)) {
                        LogWarn("mvp4: decrypt failed from " + owner);
                        const auto end = std::chrono::steady_clock::now();
                        const auto latency_ms =
                                std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                        AppendDownloadStat(stats_file, chunk_id, owner, false, latency_ms, strategy);
                        continue;
                    }

                    // 对解密后的明文重新计算 SHA-256 哈希值
                    const std::string hash = Sha256Hex(plaintext);

                    // 完整性校验：对比计算出的哈希值与预期的 chunk_id
                    if (hash == chunk_id) {
                        // ✅ 校验通过，数据完整且正确

                        // 记录下载结束时间
                        const auto end = std::chrono::steady_clock::now();
                        const auto latency_ms =
                                std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

                        // 记录成功的下载统计（success=true）
                        AppendDownloadStat(stats_file, chunk_id, owner, true, latency_ms, strategy);

                        // 将解密后的数据移动到输出参数
                        *out_data = std::move(plaintext);
                        return true;
                    }
                    // 如果哈希不匹配，说明数据损坏或被篡改，继续尝试下一个节点
                }
            }
            else {
                // ===== 普通模式：明文传输 =====

                // 发送普通下载请求：GET_CHUNK <chunk_id>
                // 服务器会返回原始的 chunk 数据
                RunTcpClientSession(h, p, {"GET_CHUNK " + chunk_id}, &resps);

                // 检查响应是否存在且格式正确（应以 "CHUNK_DATA " 开头）
                if (!resps.empty() && resps[0].rfind("CHUNK_DATA ", 0) == 0) {
                    // 提取数据部分（去掉 "CHUNK_DATA " 前缀）
                    const std::string content =
                            resps[0].substr(std::string("CHUNK_DATA ").size());

                    // 对接收到的数据计算 SHA-256 哈希值
                    const std::string hash = Sha256Hex(
                        reinterpret_cast<const uint8_t *>(content.data()), content.size());

                    // 完整性校验：对比计算出的哈希值与预期的 chunk_id
                    if (hash == chunk_id) {
                        // ✅ 校验通过，数据完整且正确

                        // 记录下载结束时间
                        const auto end = std::chrono::steady_clock::now();
                        // 计算下载延迟（毫秒）
                        const auto latency_ms =
                                std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                        // 记录成功的下载统计（success=true
                        AppendDownloadStat(stats_file, chunk_id, owner, true, latency_ms, strategy);
                        // 将数据复制到输出参数
                        out_data->assign(content.begin(), content.end());
                        // 下载成功，立即返回
                        return true;
                    }
                    // 如果哈希不匹配，继续尝试下一个节点
                }
            }
            // 如果上面的下载尝试失败（连接失败、响应格式错误、校验失败等）
            // 记录失败的下载统计

            // 记录下载结束时间
            const auto end = std::chrono::steady_clock::now();

            // 计算下载延迟（毫秒）
            const auto latency_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            // 记录失败的下载统计（success=false）
            AppendDownloadStat(stats_file, chunk_id, owner, false, latency_ms, strategy);
        }
        return false;
    }


    // 函数定义：上传文件并分发副本到其它节点
    // 返回值：true=成功完成上传和副本分发，false=失败
    bool UploadAndReplicate(const std::string &input_path,
                            const std::string &output_dir,
                            const std::string &index_path,
                            const std::string &manifest_out,
                            const AppConfig &cfg,
                            int replica_count,
                            RoutingTable *table,
                            std::map<std::string, std::vector<std::string> > *dht_store) {
        //----------------------------------------------阶段1:本地文件处理与持久化-：本地文件处理与持久化
        // 核心任务：切分、 校验、保存索引和 Manifest
        //。
        //-------------------------------------------
        // 声明向量用于存储切分后的 chunk 元数据列表
        std::vector<ChunkInfo> chunks;

        // 计算每个 chunk 的目标大小（字节）
        // 配置中的 chunk_size_mb 单位是 MB，需要转换为字节
        const std::size_t chunk_size_bytes =
                static_cast<std::size_t>(cfg.chunk_size_mb) * 1024 * 1024;

        // 步骤 1：将输入文件切分为多个 chunk
        // SplitFileToChunks 会：
        //   - 按固定大小切分文件
        //   - 对每个 chunk 计算 SHA-256 哈希作为 chunk_id
        //   - 保存为 <chunk_id>.chunk 文件到 output_dir
        //   - 填充 chunks 向量（包含 chunk_id、path、size）
        if (!SplitFileToChunks(input_path, output_dir, chunk_size_bytes, &chunks)) {
            LogError("mvp5: split failed");
            return false;
        }

        // 步骤 2：为每个 chunk 添加拥有者信息和来源标签
        for (auto &info: chunks) {
            // 将当前节点地址添加到 owners 列表
            // 例如：info.owners = ["127.0.0.1:9001"]
            AddOwnerToChunk(cfg.self_addr, &info);

            // 标记 chunk 来源为 "upload"（表示这是上传产生的原始 chunk）
            info.source = "upload";
        }


        // 步骤 3：校验所有 chunk 的完整性
        // VerifyChunks 会重新计算每个 chunk 文件的 SHA-256，并与 chunk_id 对比
        // 确保切分和保存过程中数据没有损坏
        if (!VerifyChunks(chunks)) {
            LogError("mvp5: verify failed");
            return false;
        }


        // 步骤 4：保存 chunk 索引到 TSV 文件
        // SaveChunkIndex 会将 chunks 向量序列化到 index_path
        // 格式：chunk_id<TAB>path<TAB>size<TAB>owners<TAB>source
        if (!SaveChunkIndex(index_path, chunks)) {
            LogError("mvp5: index save failed");
            return false;
        }


        // 步骤 5：保存 manifest 文件（记录 chunk 的顺序）
        // 如果用户指定了 manifest_out 路径，使用它；否则默认使用 <index_path>.manifest
        const std::string manifest_path =
                manifest_out.empty() ? (index_path + ".manifest") : manifest_out;
        // SaveChunkManifest 会将 chunk_id 列表按顺序写入文件（每行一个）
        // 用于下载时按正确顺序重组文件
        if (!SaveChunkManifest(manifest_path, chunks)) {
            LogWarn("mvp5: manifest save failed: " + manifest_path);
        } else {
            LogInfo("mvp5: manifest saved: " + manifest_path);
        }

        // 步骤 6：记录上传元数据（用于统计分析）

        // 获取原始文件的大小（字节）
        std::uintmax_t file_size = 0;
        try {
            file_size = std::filesystem::file_size(input_path);
        } catch (...) {
            LogWarn("upload meta: cannot read file size");
        }

        // 提取文件名（不含路径）
        const std::string file_name =
                std::filesystem::path(input_path).filename().string();


        // 追加上传元数据到统计文件
        // 记录：文件名、文件大小、上传者地址、manifest 路径
        AppendUploadMeta(cfg.upload_meta_file, file_name, file_size,
                         cfg.self_addr, manifest_path);
        //----------------------------------------------副本分发 (Replication)
        //核心任务：将 chunk 数据发送给其他节点，并更新本地的 owners 记录

        // 步骤 7：如果指定了副本数量，执行副本分发
        if (replica_count > 0) {
            LogInfo("mvp5: replica dispatch start, count=" + std::to_string(replica_count));
            // 声明目标节点列表
            std::vector<std::string> targets;

            // 从种子节点中选择副本目标（排除自己
            for (const auto &seed: cfg.seed_nodes) {
                if (seed == cfg.self_addr) {
                    continue; // 跳过自己，不给自己发副本
                }
                targets.push_back(seed); // 加入目标列表

                // 如果已达到指定的副本数量，停止选择
                if (static_cast<int>(targets.size()) >= replica_count) {
                    break;
                }
            }
            // 遍历每个目标节点，发送所有 chunk 的副本
            for (const auto &t: targets) {
                // 遍历每个 chunk，发送到当前目标节点
                for (auto &info: chunks) {
                    // 发送 chunk 副本到目标节点 t
                    // SendChunkReplica 会：
                    //   - 读取 chunk 文件内容
                    //   - 通过 TCP 发送 PUT_CHUNK_HEX 请求
                    //   - 返回是否成功
                    const bool ok = SendChunkReplica(t, info);


                    // 记录副本分发结果到统计文件
                    // 格式：chunk_id<TAB>target_node<TAB>success/failure
                    AppendReplicaResult(cfg.upload_replica_file, info.chunk_id, t, ok);

                    // 如果发送成功，更新本地记录
                    if (ok) {
                        // 将目标节点添加到该 chunk 的 owners 列表
                        // 例如：info.owners = ["127.0.0.1:9001", "127.0.0.1:9002"]
                        AddOwnerToChunk(t, &info);

                        // 更新索引文件（Upsert = Update or Insert）
                        // 如果 chunk 已存在，合并 owners；如果不存在，新增记录
                        UpsertChunkIndex(index_path, info);
                    }
                }
            }
            LogInfo("mvp5: replica dispatch done");
        }
        //----------------------------------------------DHT将 chunk 的元数据（位置信息）广播到 P2P 网络。
        // 步骤 8：将 chunk 信息发布到 DHT 网络（如果提供了路由表和 DHT 存储）
        if (table && dht_store) {
            // 遍历所有 chunk
            for (const auto &info: chunks) {
                // 8a. 更新本地 DHT 存储
                // 将 chunk_id → owners 映射存入内存中的 dht_store（就是那个map）
                // 这样当前节点可以快速响应其他节点的 FIND_VALUE 查询
                UpsertDhtValue(dht_store, info.chunk_id, info.owners);

                // 8b. 发布到 P2P 网络
                // StoreValueToDht 会：
                //   - 通过 IterativeFindNodes 找到距离 chunk_id 最近的 K 个节点
                //   - 向这些节点发送 STORE 请求
                //   - 让网络中的多个节点都知道这个 chunk 的位置
                //  更新其他节点内存里的那个map
                StoreValueToDht(info.chunk_id, info.owners, table,
                                cfg.seed_nodes, cfg.routing_capacity);
            }
        }
        return true;
        /*阶段 1：本地处理（1246-1281行）
切分文件 → 生成 chunk 文件
添加元数据 → 标记拥有者和来源
完整性校验 → 确保数据正确
保存索引 → 写入 TSV 文件
保存 manifest → 记录 chunk 顺序
记录统计 → 写入上传元数据日志
         *
         *
        *阶段 2：副本分发（1283-1306行）
选择目标节点 → 从种子节点中选 replica_count 个
发送副本 → 逐个 chunk 发送到每个目标节点
更新记录 → 成功后更新 owners 列表和索引文件
记录结果 → 写入副本分发统计日志
         *
        *DHT 发布（1307-1313行）
更新本地缓存 → 填充 dht_store
发布到网络 → 通过 STORE 协议让其他节点知道 chunk 位置
         */
    }


    // 函数作用：通过manifest清单，下载所有分片并拼接成完整文件
    bool DownloadByManifest(const std::string &manifest_in,
                            const std::string &out_file,
                            const std::string &peer,
                            const AppConfig &cfg,
                            const std::string &strategy,
                            bool secure_mode,
                            RoutingTable *table) {

        // 1. 定义容器：存储manifest里的所有分片ID
        std::vector<std::string> chunk_ids;

        // 2. 加载manifest清单文件 → 读取所有分片ID
        // 清单为空/加载失败 → 直接报错返回
        if (!LoadChunkManifest(manifest_in, &chunk_ids) || chunk_ids.empty()) {
            LogError("mvp5: manifest empty or not found");
            return false;
        }

        // 3. 以二进制模式打开输出文件（清空原有内容，准备写入完整文件）
        std::ofstream out(out_file, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            LogError("mvp5: cannot open out_file " + out_file);
            return false;
        }


        // 4. 安全模式：如果开启加密，加载AES/hmac密钥（后续解密分片用）
        std::vector<uint8_t> aes_key;
        std::vector<uint8_t> hmac_key;
        if (secure_mode) {
            if (!LoadCryptoKeys(cfg, &aes_key, &hmac_key)) {
                return false;
            }
            LogInfo("mvp4: secure mode enabled");
        }

        // ====================== 核心循环：遍历每一个分片ID ======================
        for (const auto &id: chunk_ids) {
            // 5. 查找当前分片的「拥有者节点」（谁存储了这个分片）
            std::vector<std::string> owners =
                    IterativeFindValue(id, table, cfg.seed_nodes, cfg.routing_capacity, 8);


            // 6. 如果路由表没找到 → 主动向种子节点发送请求，查询分片拥有者
            if (owners.empty()) {
                std::vector<std::string> candidates = cfg.seed_nodes;
                if (candidates.empty()) {
                    candidates.push_back(peer);
                }
                if (std::find(candidates.begin(), candidates.end(), peer) == candidates.end()) {
                    candidates.push_back(peer);
                }
                for (const auto &c: candidates) {
                    std::string h2;
                    int p2 = 0;
                    if (!ParseHostPort(c, &h2, &p2)) {
                        continue;
                    }


                    std::vector<std::string> resps;
                    RunTcpClientSession(h2, p2, {"FIND_CHUNK " + id}, &resps);
                    if (!resps.empty() && resps[0].rfind("FIND_RESP", 0) == 0) {
                        const std::string list = TrimLocal(
                            resps[0].substr(std::string("FIND_RESP").size()));
                        for (const auto &item: SplitList(list)) {
                            if (!item.empty()) {
                                owners.push_back(item);
                            }
                        }
                    }
                }
            }

            // 7. 去重、排序拥有者 → 应用下载策略（比如优先选哪个节点下载）
            std::sort(owners.begin(), owners.end());
            owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
            ApplyDownloadStrategy(strategy, &owners);

            // 8. 调用底层函数：从拥有者节点下载这个分片的数据
            std::vector<uint8_t> data;
            if (!DownloadChunkByOwners(owners, id, secure_mode,
                                       aes_key, hmac_key, cfg.self_addr,
                                       cfg.download_stats_file, strategy, &data)) {
                LogError("mvp5: download chunk failed id=" + id);
                return false;
            }
            out.write(reinterpret_cast<const char *>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        }
        LogInfo("mvp5: file assembled: " + out_file);
        return true;
    }
} // namespace

int main(int argc, char **argv) {
    //----------------------------------------------------------*读取配置和日志
    // 1) 读取配置
    const std::string config_path = ParseConfigPath(argc, argv);
    const AppConfig cfg = LoadConfig(config_path);

    // 2) 初始化日志（确保日志目录存在）
    if (!cfg.log_file.empty()) {
        std::filesystem::path log_path(cfg.log_file);
        if (log_path.has_parent_path()) {
            std::filesystem::create_directories(log_path.parent_path());
        }
    }

    LoggerConfig log_cfg;
    log_cfg.level = ParseLogLevel(cfg.log_level);
    log_cfg.file_path = cfg.log_file;
    InitLogger(log_cfg);

    // 3) 启动日志与配置输出
    LogInfo("app start");
    LogInfo("current time: " + CurrentTimeString());
    LogInfo("config path: " + config_path);
    PrintConfig(cfg);
    // ----------------------------------------------------------*读取配置和日志
    // MVP2/MVP5-1：若传入 --input/--upload，则执行分片、校验并保存索引
    //要上传/分片的文件路径
    const std::string input_path = ParseArgValue(argc, argv, "--input");
    //作用：也是指定要上传的文件路径（与 --input 功能相同）
    //用途：可能是为了语义更清晰而提供的别名参
    const std::string upload_path = ParseArgValue(argc, argv, "--upload");

    //作用，副本数量
    const int replica_count =
            ParseIntOrDefault(ParseArgValue(argc, argv, "--replica"), 0);


    const std::string output_dir_arg = ParseArgValue(argc, argv, "--chunks_dir");

    //output_dir是output_dir_arg的最终版（真正用的）
    // 上传时：分片保存到哪里 ，. 下载时：分片保存到哪里，。 接收副本时：副本分片保存到哪里
    const std::string output_dir = output_dir_arg.empty() ? cfg.chunks_dir : output_dir_arg;

    //解析索引文件路径 (第1286-1288行)
    const std::string index_path_arg = ParseArgValue(argc, argv, "--chunk_index_file");
    /*
    *完全正确！ 你的理解非常准确。
核心要点
1. 一个节点 = 一个 TSV 文件
每个节点有自己独立的 chunk_index.tsv 文件，路径由配置决定：
tsv
     *
     */
    const std::string index_path =
            index_path_arg.empty() ? cfg.chunk_index_file : index_path_arg;

    //
    const std::string manifest_out =
            ParseArgValue(argc, argv, "--manifest_out");

    /*确定最终输入文件 (第1294-1297行)
    *逻辑：
优先使用 input_path（即 --input 参数）
如果 --input 为空，才使用 upload_path（即 --upload 参数）
如果两者都提供了 → 记录警告日志，但仍优先用 --input
     *
     *
     */
    const std::string chosen_input = input_path.empty() ? upload_path : input_path;
    if (!input_path.empty() && !upload_path.empty()) {
        LogWarn("mvp5: both --input and --upload provided, using --input");
    }
    //-------------------------------------------------------------在这下面配置读取就结束了
    if (!chosen_input.empty()) {
        //------------------------./build/app --config "data/full_e2e/configs/node_1.yaml" \
        --upload "data/full_e2e/input.txt" --replica 5 \
        --manifest_out "data/full_e2e/manifest.txt" 在这里就会执行
        LogInfo("mvp5: upload/local split requested");
        if (!UploadAndReplicate(chosen_input, output_dir, index_path,
                                manifest_out, cfg, replica_count,
                                nullptr, nullptr)) {
            return 1;
        }
    } else {
        // ----------------------------没有输入文件时，尝试加载索引并校验分片（实际上就是啥也不做）
        std::vector<ChunkInfo> loaded;
        if (LoadChunkIndex(index_path, &loaded)) {
            VerifyChunks(loaded);
        }
    }

    // MVP3：简化路由表（k-bucket 雏形）
    RoutingTable table(cfg.self_addr, cfg.routing_capacity);
    for (const auto &seed: cfg.seed_nodes) {
        table.AddNode(seed);
    }
    // 将自身地址也纳入候选列表（便于 FIND_CHUNK 返回）
    table.AddNode(cfg.self_addr);
    //--------------------------------------------------------------------命令手动添加/删除节点
    const std::string add_node = ParseArgValue(argc, argv, "--add_node");
    if (!add_node.empty()) {
        table.AddNode(add_node);
    }
    const std::string remove_node = ParseArgValue(argc, argv, "--remove_node");
    if (!remove_node.empty()) {
        table.RemoveNode(remove_node);
    }
    LogInfo("routing table snapshot:");
    for (const auto &n: table.Snapshot()) {
        LogInfo("route: key=" + n.key + " dist=" + std::to_string(n.dist));
    }

    //--------------------------------------------------------------------命令手动添加/删除节点
    std::map<std::string, std::vector<std::string> > dht_store; /*Key: chunk_id（分块的 SHA-256 哈希字符串）
Value: vector<string>（拥有该分块的节点地址列表，如 ["127.0.0.1:9001", "127.0.0.1:9002"]）*/ {
        std::vector<ChunkInfo> local_chunks;
        if (LoadChunkIndex(index_path, &local_chunks)) {
            //就是为了赋值这个map，不做网络传输
            for (const auto &info: local_chunks) {
                UpsertDhtValue(&dht_store, info.chunk_id, info.owners);
            }
        }
    }
    if (!chosen_input.empty()) {
        //------------------------------------------------------------- node1上传
        //./build/app --config "data/full_e2e/configs/node_1.yaml" \
        --upload "data/full_e2e/input.txt" --replica 5 \
        --manifest_out "data/full_e2e/manifest.txt"
        std::vector<ChunkInfo> uploaded;
        if (LoadChunkIndex(index_path, &uploaded)) {
            for (const auto &info: uploaded) {
                StoreValueToDht(info.chunk_id, info.owners, &table,
                                cfg.seed_nodes, cfg.routing_capacity);
            }
        }
    }

    // MVP3：FIND_CHUNK 查询 -> 返回节点列表 -> 下载校验
    const std::string mode = ParseArgValue(argc, argv, "--mode");
    const bool secure_mode = HasFlag(argc, argv, "--secure");
    if (mode == "server") {
        //------------------------------------------------------------- node1~node 6服务器

        //还可以大量进入的
        const std::string bind_host = "0.0.0.0";
        std::vector<ChunkInfo> loaded;
        LoadChunkIndex(index_path, &loaded);
        std::vector<uint8_t> aes_key;
        std::vector<uint8_t> hmac_key;
        const bool crypto_ready = LoadCryptoKeys(cfg, &aes_key, &hmac_key);
        auto handler = [&loaded, &cfg, &table, &dht_store, &aes_key, &hmac_key, crypto_ready](
            const std::string &req) -> std::string {
            if (req == "NODE_HELLO") {
                return "NODE_WELCOME";
            }
            if (req.rfind("FIND_NODE ", 0) == 0) {
                const std::string target = req.substr(std::string("FIND_NODE ").size());
                const auto keys = table.TopKClosestKeys(target, cfg.routing_capacity);
                const std::string joined = JoinList(keys);
                return joined.empty() ? "FIND_NODE_RESP" : ("FIND_NODE_RESP " + joined);
            }
            if (req.rfind("FIND_VALUE ", 0) == 0) {
                const std::string key = req.substr(std::string("FIND_VALUE ").size());
                auto it = dht_store.find(key);
                if (it != dht_store.end() && !it->second.empty()) {
                    return "FIND_VALUE_RESP " + JoinList(it->second);
                }
                const auto keys = table.TopKClosestKeys(key, cfg.routing_capacity);
                const std::string joined = JoinList(keys);
                return joined.empty() ? "FIND_NODE_RESP" : ("FIND_NODE_RESP " + joined);
            }
            if (req.rfind("STORE ", 0) == 0) {
                const std::string payload = req.substr(std::string("STORE ").size());
                std::istringstream iss(payload);
                std::string key;
                std::string owners_csv;
                if (!(iss >> key >> owners_csv)) {
                    return "STORE_ERROR";
                }
                const auto owners = SplitList(owners_csv);
                UpsertDhtValue(&dht_store, key, owners);
                for (const auto &o: owners) {
                    table.AddNode(o);
                }
                return "STORE_OK";
            }
            if (req.rfind("FIND_CHUNK ", 0) == 0) {
                const std::string chunk_id = req.substr(std::string("FIND_CHUNK ").size());
                std::vector<std::string> owners;
                if (FindChunkOwners(loaded, chunk_id, &owners) && !owners.empty()) {
                    return "FIND_RESP " + JoinList(owners);
                }
                // 没有 owner 记录时，退回到路由表候选列表
                const auto keys = table.TopKKeys(3);
                const std::string joined = JoinList(keys);
                return joined.empty() ? "FIND_RESP" : ("FIND_RESP " + joined);
            }
            if (req.rfind("GET_CHUNK ", 0) == 0) {
                const std::string chunk_id = req.substr(std::string("GET_CHUNK ").size());
                std::string path;
                if (!FindChunkPath(loaded, chunk_id, &path)) {
                    return "CHUNK_NOT_FOUND";
                }
                std::ifstream in(path, std::ios::binary);
                if (!in.is_open()) {
                    return "CHUNK_READ_ERROR";
                }
                std::string data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                return "CHUNK_DATA " + data;
            }
            if (req.rfind("PUT_CHUNK_HEX ", 0) == 0) {
                const std::string payload = req.substr(std::string("PUT_CHUNK_HEX ").size());
                const auto space_pos = payload.find(' ');
                if (space_pos == std::string::npos) {
                    return "PUT_ERROR";
                }
                const std::string chunk_id = payload.substr(0, space_pos);
                const std::string hex = payload.substr(space_pos + 1);
                std::vector<uint8_t> data;
                if (!DecodeHex(hex, &data)) {
                    return "PUT_ERROR";
                }
                ChunkInfo info;
                if (!SaveChunkFile(chunk_id, data, cfg.chunks_dir, &info)) {
                    return "PUT_ERROR";
                }
                AddOwnerToChunk(cfg.self_addr, &info);
                info.source = "replica";
                if (!UpsertChunkIndex(cfg.chunk_index_file, info)) {
                    return "PUT_ERROR";
                }
                LoadChunkIndex(cfg.chunk_index_file, &loaded);
                UpsertDhtValue(&dht_store, info.chunk_id, info.owners);
                LogInfo("mvp5: chunk stored via PUT_CHUNK_HEX id=" + chunk_id +
                        " size=" + std::to_string(info.size));
                return "PUT_OK";
            }
            if (req.rfind("PUT_CHUNK_HEX2 ", 0) == 0) {
                const std::string payload = req.substr(std::string("PUT_CHUNK_HEX2 ").size());
                std::istringstream iss(payload);
                std::string chunk_id;
                std::string owners_csv;
                std::string hex;
                if (!(iss >> chunk_id >> owners_csv >> hex)) {
                    return "PUT_ERROR";
                }
                std::vector<uint8_t> data;
                if (!DecodeHex(hex, &data)) {
                    return "PUT_ERROR";
                }
                ChunkInfo info;
                if (!SaveChunkFile(chunk_id, data, cfg.chunks_dir, &info)) {
                    return "PUT_ERROR";
                }
                info.owners = SplitList(owners_csv);
                AddOwnerToChunk(cfg.self_addr, &info);
                info.source = "replica";
                if (!UpsertChunkIndex(cfg.chunk_index_file, info)) {
                    return "PUT_ERROR";
                }
                LoadChunkIndex(cfg.chunk_index_file, &loaded);
                UpsertDhtValue(&dht_store, info.chunk_id, info.owners);
                LogInfo("mvp5: chunk stored via PUT_CHUNK_HEX2 id=" + chunk_id +
                        " size=" + std::to_string(info.size));
                return "PUT_OK";
            }
            if (req.rfind("GET_CHUNK_SEC ", 0) == 0) {
                if (!crypto_ready) {
                    return "CHUNK_SEC_ERROR";
                }
                const std::string chunk_id = req.substr(std::string("GET_CHUNK_SEC ").size());
                std::string path;
                if (!FindChunkPath(loaded, chunk_id, &path)) {
                    return "CHUNK_SEC_NOT_FOUND";
                }
                std::ifstream in(path, std::ios::binary);
                if (!in.is_open()) {
                    return "CHUNK_SEC_READ_ERROR";
                }
                std::string data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                const std::vector<uint8_t> plaintext(
                    reinterpret_cast<const uint8_t *>(data.data()),
                    reinterpret_cast<const uint8_t *>(data.data()) + data.size());
                std::string secure_text;
                if (!EncryptToSecureText(aes_key, hmac_key, plaintext, &secure_text)) {
                    return "CHUNK_SEC_ENCRYPT_ERROR";
                }
                return "CHUNK_SEC " + secure_text;
            }
            return "UNKNOWN_REQUEST";
        };
        //RunTcpServerOnceWithHandler 这个方法，就看这一次就可以
        RunTcpServerOnceWithHandler(bind_host, cfg.listen_port, handler);
    }
    else if (mode == "console") {
        LogInfo("mvp5: console mode (server + interactive upload/download)");
        const std::string bind_host = "0.0.0.0";
        std::vector<ChunkInfo> loaded;
        LoadChunkIndex(index_path, &loaded);
        std::vector<uint8_t> aes_key;
        std::vector<uint8_t> hmac_key;
        const bool crypto_ready = LoadCryptoKeys(cfg, &aes_key, &hmac_key);
        std::thread server_thread([&]() {
            auto handler = [&loaded, &cfg, &table, &dht_store, &aes_key, &hmac_key, crypto_ready](
                const std::string &req) -> std::string {
                if (req == "NODE_HELLO") {
                    return "NODE_WELCOME";
                }
                if (req.rfind("FIND_NODE ", 0) == 0) {
                    const std::string target = req.substr(std::string("FIND_NODE ").size());
                    const auto keys = table.TopKClosestKeys(target, cfg.routing_capacity);
                    const std::string joined = JoinList(keys);
                    return joined.empty() ? "FIND_NODE_RESP" : ("FIND_NODE_RESP " + joined);
                }
                if (req.rfind("FIND_VALUE ", 0) == 0) {
                    const std::string key = req.substr(std::string("FIND_VALUE ").size());
                    auto it = dht_store.find(key);
                    if (it != dht_store.end() && !it->second.empty()) {
                        return "FIND_VALUE_RESP " + JoinList(it->second);
                    }
                    const auto keys = table.TopKClosestKeys(key, cfg.routing_capacity);
                    const std::string joined = JoinList(keys);
                    return joined.empty() ? "FIND_NODE_RESP" : ("FIND_NODE_RESP " + joined);
                }
                if (req.rfind("STORE ", 0) == 0) {
                    const std::string payload = req.substr(std::string("STORE ").size());
                    std::istringstream iss(payload);
                    std::string key;
                    std::string owners_csv;
                    if (!(iss >> key >> owners_csv)) {
                        return "STORE_ERROR";
                    }
                    const auto owners = SplitList(owners_csv);
                    UpsertDhtValue(&dht_store, key, owners);
                    for (const auto &o: owners) {
                        table.AddNode(o);
                    }
                    return "STORE_OK";
                }
                if (req.rfind("FIND_CHUNK ", 0) == 0) {
                    const std::string chunk_id = req.substr(std::string("FIND_CHUNK ").size());
                    std::vector<std::string> owners;
                    if (FindChunkOwners(loaded, chunk_id, &owners) && !owners.empty()) {
                        return "FIND_RESP " + JoinList(owners);
                    }
                    const auto keys = table.TopKKeys(3);
                    const std::string joined = JoinList(keys);
                    return joined.empty() ? "FIND_RESP" : ("FIND_RESP " + joined);
                }
                if (req.rfind("GET_CHUNK ", 0) == 0) {
                    const std::string chunk_id = req.substr(std::string("GET_CHUNK ").size());
                    std::string path;
                    if (!FindChunkPath(loaded, chunk_id, &path)) {
                        return "CHUNK_NOT_FOUND";
                    }
                    std::ifstream in(path, std::ios::binary);
                    if (!in.is_open()) {
                        return "CHUNK_READ_ERROR";
                    }
                    std::string data((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
                    return "CHUNK_DATA " + data;
                }
                if (req.rfind("PUT_CHUNK_HEX ", 0) == 0) {
                    const std::string payload = req.substr(std::string("PUT_CHUNK_HEX ").size());
                    const auto space_pos = payload.find(' ');
                    if (space_pos == std::string::npos) {
                        return "PUT_ERROR";
                    }
                    const std::string chunk_id = payload.substr(0, space_pos);
                    const std::string hex = payload.substr(space_pos + 1);
                    std::vector<uint8_t> data;
                    if (!DecodeHex(hex, &data)) {
                        return "PUT_ERROR";
                    }
                    ChunkInfo info;
                    if (!SaveChunkFile(chunk_id, data, cfg.chunks_dir, &info)) {
                        return "PUT_ERROR";
                    }
                    AddOwnerToChunk(cfg.self_addr, &info);
                    info.source = "replica";
                    if (!UpsertChunkIndex(cfg.chunk_index_file, info)) {
                        return "PUT_ERROR";
                    }
                    loaded.push_back(info);
                    UpsertDhtValue(&dht_store, info.chunk_id, info.owners);
                    LogInfo("mvp5: chunk stored via PUT_CHUNK_HEX id=" + chunk_id +
                            " size=" + std::to_string(info.size));
                    return "PUT_OK";
                }
                if (req.rfind("PUT_CHUNK_HEX2 ", 0) == 0) {
                    const std::string payload = req.substr(std::string("PUT_CHUNK_HEX2 ").size());
                    std::istringstream iss(payload);
                    std::string chunk_id;
                    std::string owners_csv;
                    std::string hex;
                    if (!(iss >> chunk_id >> owners_csv >> hex)) {
                        return "PUT_ERROR";
                    }
                    std::vector<uint8_t> data;
                    if (!DecodeHex(hex, &data)) {
                        return "PUT_ERROR";
                    }
                    ChunkInfo info;
                    if (!SaveChunkFile(chunk_id, data, cfg.chunks_dir, &info)) {
                        return "PUT_ERROR";
                    }
                    info.owners = SplitList(owners_csv);
                    AddOwnerToChunk(cfg.self_addr, &info);
                    info.source = "replica";
                    if (!UpsertChunkIndex(cfg.chunk_index_file, info)) {
                        return "PUT_ERROR";
                    }
                    loaded.push_back(info);
                    UpsertDhtValue(&dht_store, info.chunk_id, info.owners);
                    LogInfo("mvp5: chunk stored via PUT_CHUNK_HEX2 id=" + chunk_id +
                            " size=" + std::to_string(info.size));
                    return "PUT_OK";
                }
                if (req.rfind("GET_CHUNK_SEC ", 0) == 0) {
                    if (!crypto_ready) {
                        return "CHUNK_SEC_ERROR";
                    }
                    const std::string chunk_id = req.substr(std::string("GET_CHUNK_SEC ").size());
                    std::string path;
                    if (!FindChunkPath(loaded, chunk_id, &path)) {
                        return "CHUNK_SEC_NOT_FOUND";
                    }
                    std::ifstream in(path, std::ios::binary);
                    if (!in.is_open()) {
                        return "CHUNK_SEC_READ_ERROR";
                    }
                    std::string data((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
                    const std::vector<uint8_t> plaintext(
                        reinterpret_cast<const uint8_t *>(data.data()),
                        reinterpret_cast<const uint8_t *>(data.data()) + data.size());
                    std::string secure_text;
                    if (!EncryptToSecureText(aes_key, hmac_key, plaintext, &secure_text)) {
                        return "CHUNK_SEC_ENCRYPT_ERROR";
                    }
                    return "CHUNK_SEC " + secure_text;
                }
                return "UNKNOWN_REQUEST";
            };
            RunTcpServerOnceWithHandler(bind_host, cfg.listen_port, handler);
        });

        LogInfo("console: enter command (help/exit/upload/download_manifest)");
        for (;;) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                break;
            }
            line = TrimLocal(line);
            if (line.empty()) {
                continue;
            }
            if (line == "exit" || line == "quit") {
                LogInfo("console: exit requested");
                break;
            }
            if (line == "help") {
                LogInfo("console commands:");
                LogInfo("  upload <file_path> [replica=0] [manifest_out=path]");
                LogInfo("  download_manifest <manifest_path> <out_file> <peer> [strategy=random] [secure=0]");
                LogInfo("  exit");
                continue;
            }
            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;
            if (cmd == "upload") {
                std::string file_path;
                iss >> file_path;
                int replica = 0;
                iss >> replica;
                std::string manifest_out;
                iss >> manifest_out;
                if (file_path.empty()) {
                    LogWarn("console: upload requires file_path");
                    continue;
                }
                if (!UploadAndReplicate(file_path, cfg.chunks_dir, index_path,
                                        manifest_out, cfg, replica,
                                        &table, &dht_store)) {
                    LogError("console: upload failed");
                } else {
                    LoadChunkIndex(index_path, &loaded);
                    LogInfo("console: upload done");
                }
                continue;
            }
            if (cmd == "download_manifest") {
                std::string manifest_path;
                std::string out_file;
                std::string peer;
                iss >> manifest_path >> out_file >> peer;
                std::string strategy = cfg.download_strategy;
                int secure_flag = 0;
                if (iss >> strategy) {
                    iss >> secure_flag;
                }
                if (manifest_path.empty() || out_file.empty() || peer.empty()) {
                    LogWarn("console: download_manifest requires manifest_path out_file peer");
                    continue;
                }
                if (!DownloadByManifest(manifest_path, out_file, peer, cfg,
                                        strategy, secure_flag != 0, &table)) {
                    LogError("console: download failed");
                } else {
                    LogInfo("console: download done");
                }
                continue;
            }
            LogWarn("console: unknown command, type help");
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
    } else if (mode == "client") {
        //------------------------------------------------------------- node5/node6下载
        // 1. 解析命令行参数：目标节点地址（格式 host:port）存储要连接的分布式节点地址
        const std::string peer = ParseArgValue(argc, argv, "--peer");

        // 定义变量：存储解析后的 主机名/IP 和 端口
        std::string host;
        int port = 0;

        // 解析地址：把 peer 的 "host:port" 拆分成 host 和 port
        if (!ParseHostPort(peer, &host, &port)) {
            LogError("mvp3: invalid --peer, expected host:port");
            return 1;
        }

        // 解析命令行参数：下载策略（--strategy）
        const std::string strategy_arg = ParseArgValue(argc, argv, "--strategy");

        // 确定最终下载策略：命令行优先，无则用配置文件cfg的默认策略
        const std::string strategy =
                strategy_arg.empty() ? cfg.download_strategy : strategy_arg;
        LogInfo("mvp5: strategy=" + strategy);

        // 定义加密密钥：AES加密密钥、HMAC校验密钥（字节数组）
        std::vector<uint8_t> aes_key;
        std::vector<uint8_t> hmac_key;

        // 判断：是否开启【安全加密模式】
        if (secure_mode) {
            // 加载配置文件中的加密密钥，加载失败则退出
            if (!LoadCryptoKeys(cfg, &aes_key, &hmac_key)) {
                return 1;
            }
            LogInfo("mvp4: secure mode enabled");
        }
        // 解析命令行参数：要下载的【单个数据块ID】（--chunk_id）
        const std::string chunk_id = ParseArgValue(argc, argv, "--chunk_id");

        // 解析命令行参数：数据块清单文件路径（--manifest，批量下载用）
        const std::string manifest_in = ParseArgValue(argc, argv, "--manifest");

        // 解析命令行参数：批量下载后的输出文件路径（--out_file）
        const std::string out_file = ParseArgValue(argc, argv, "--out_file");

        // 判断：既没有单个chunk，也没有清单 → 执行节点握手
        if (chunk_id.empty() && manifest_in.empty()) {
            // 定义容器：存储TCP通信的响应结果
            std::vector<std::string> resps;

            // 建立TCP连接，发送 NODE_HELLO 节点握手指令
            RunTcpClientSession(host, port, {"NODE_HELLO"}, &resps);

            // 如果收到响应，打印响应内容
            if (!resps.empty()) {
                LogInfo("mvp3: hello response=" + resps[0]);
            }
        } else {
            // 判断：传入了数据块ID → 单个chunk下载
            if (!chunk_id.empty()) {
                // 第一步：分布式查找【存储这个chunk的节点列表】
                std::vector<std::string> owners =
                        IterativeFindValue(chunk_id, &table, cfg.seed_nodes, cfg.routing_capacity, 8);

                // 判断：本地路由表没找到节点 → 主动向网络中查询
                if (owners.empty()) {
                    // 候选节点：用配置的种子节点
                    std::vector<std::string> candidates = cfg.seed_nodes;
                    // 种子节点为空 → 用当前peer作为候选
                    if (candidates.empty()) {
                        candidates.push_back(peer);
                    }
                    // 把当前peer加入候选（去重，避免重复）
                    if (std::find(candidates.begin(), candidates.end(), peer) == candidates.end()) {
                        candidates.push_back(peer);
                    }
                    // 遍历所有候选节点，发送查询指令
                    for (const auto &c: candidates) {
                        std::string h;
                        int p = 0;
                        if (!ParseHostPort(c, &h, &p)) {
                            continue;
                        }
                        std::vector<std::string> resps;

                        // 发送指令：FIND_CHUNK 查找目标数据块
                        RunTcpClientSession(h, p, {"FIND_CHUNK " + chunk_id}, &resps);

                        // 解析响应：收到 FIND_RESP 则提取存储节点
                        if (!resps.empty() && resps[0].rfind("FIND_RESP", 0) == 0) {
                            // 截取响应内容，去掉指令头
                            const std::string list = TrimLocal(
                                resps[0].substr(std::string("FIND_RESP").size()));
                            // 拆分响应列表，把有效节点加入owners
                            for (const auto &item: SplitList(list)) {
                                if (!item.empty()) {
                                    owners.push_back(item);
                                }
                            }
                        }
                    }
                }
                // 节点列表：排序 + 去重（避免重复下载）
                std::sort(owners.begin(), owners.end());
                owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
                // 应用下载策略：筛选/排序节点（比如选最快的节点）
                ApplyDownloadStrategy(strategy, &owners);
                // 判断：没有找到任何存储节点 → 打印警告
                if (owners.empty()) {
                    LogWarn("mvp3: no owners found for chunk");
                } else {
                    // 定义容器：存储下载的二进制数据
                    std::vector<uint8_t> data;
                    // 核心函数：从节点列表下载数据块（支持加密）
                    if (DownloadChunkByOwners(owners, chunk_id, secure_mode,
                                              aes_key, hmac_key, cfg.self_addr,
                                              cfg.download_stats_file, strategy, &data)) {
                        LogInfo("mvp5: download ok bytes=" + std::to_string(data.size()));
                    } else {
                        LogWarn("mvp3: download failed");
                    }
                }
            }
        }
        // 判断：传入了数据块清单 → 批量下载并组装成完整文件
        if (!manifest_in.empty()) {
            // 必须指定输出文件，否则报错退出
            if (out_file.empty()) {
                LogError("mvp5: --out_file is required for --manifest");
                return 1;
            }
            // 存储清单中的所有chunk ID
            std::vector<std::string> chunk_ids;

            // 读取清单文件，加载所有数据块ID，失败则退出
            if (!LoadChunkManifest(manifest_in, &chunk_ids) || chunk_ids.empty()) {
                LogError("mvp5: manifest empty or not found");
                return 1;
            }
            // 二进制模式打开输出文件（清空原有内容）（这里是先准备输出文件，再开始下载内容并且搞到输出文件里）
            std::ofstream out(out_file, std::ios::binary | std::ios::trunc);

            // 文件打开失败 → 报错退出
            if (!out.is_open()) {
                LogError("mvp5: cannot open out_file " + out_file);
                return 1;
            }

            // 遍历清单中的每一个数据块ID → 逐个下载
            for (const auto &id: chunk_ids) {
                // ====================== 重复逻辑 ======================
                // 1. 查找存储节点（本地路由+网络查询）
                std::vector<std::string> owners =
                        IterativeFindValue(id, &table, cfg.seed_nodes, cfg.routing_capacity, 8);
                if (owners.empty()) {
                    std::vector<std::string> candidates = cfg.seed_nodes;
                    if (candidates.empty()) {
                        candidates.push_back(peer);
                    }
                    if (std::find(candidates.begin(), candidates.end(), peer) == candidates.end()) {
                        candidates.push_back(peer);
                    }
                    for (const auto &c: candidates) {
                        std::string h2;
                        int p2 = 0;
                        if (!ParseHostPort(c, &h2, &p2)) {
                            continue;
                        }
                        std::vector<std::string> resps;
                        RunTcpClientSession(h2, p2, {"FIND_CHUNK " + id}, &resps);
                        if (!resps.empty() && resps[0].rfind("FIND_RESP", 0) == 0) {
                            const std::string list = TrimLocal(
                                resps[0].substr(std::string("FIND_RESP").size()));
                            for (const auto &item: SplitList(list)) {
                                if (!item.empty()) {
                                    owners.push_back(item);
                                }
                            }
                        }
                    }
                }
                // 2. 节点去重+排序
                std::sort(owners.begin(), owners.end());
                owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
                // 3. 应用下载策略
                ApplyDownloadStrategy(strategy, &owners);
                std::vector<uint8_t> data;
                // 下载当前数据块
                if (!DownloadChunkByOwners(owners, id, secure_mode,
                                           aes_key, hmac_key, cfg.self_addr,
                                           cfg.download_stats_file, strategy, &data)) {
                    // 任意一个chunk下载失败 → 直接退出
                    LogError("mvp5: download chunk failed id=" + id);
                    return 1;
                }
                out.write(reinterpret_cast<const char *>(data.data()),
                          static_cast<std::streamsize>(data.size()));
            }
            LogInfo("mvp5: file assembled: " + out_file);
        }
    } else if (mode == "stats") {
        //------./build/app --config "data/full_e2e/configs/node_5.yaml" --mode stats --stats_type download --json --limit 10 > "logs/full_stats_download.json"
        //终端A查询
        const std::string stats_type = ParseArgValue(argc, argv, "--stats_type");
        const std::string owner_filter = ParseArgValue(argc, argv, "--owner");
        const std::string limit_str = ParseArgValue(argc, argv, "--limit");
        const bool json_out = HasFlag(argc, argv, "--json");
        const int limit = ParseIntOrDefault(limit_str, 50);

        if (stats_type == "download") {
            std::vector<DownloadStatRow> rows;
            LoadDownloadStats(cfg.download_stats_file, &rows);
            struct Agg {
                int total = 0;
                int success = 0;
                long latency_sum = 0;
            };
            std::map<std::string, Agg> agg;
            std::map<std::string, std::string> last_time;
            for (const auto &r: rows) {
                if (!owner_filter.empty() && r.owner != owner_filter) {
                    continue;
                }
                auto &entry = agg[r.owner];
                entry.total += 1;
                entry.success += (r.success ? 1 : 0);
                entry.latency_sum += r.latency_ms;
                last_time[r.owner] = r.time;
            }
            if (json_out) {
                std::ostringstream oss;
                oss << "{";
                oss << "\"type\":\"download\",";
                oss << "\"total_rows\":" << rows.size() << ",";
                oss << "\"items\":[";
                bool first = true;
                int count = 0;
                for (const auto &r: rows) {
                    if (!owner_filter.empty() && r.owner != owner_filter) {
                        continue;
                    }
                    if (count >= limit) {
                        break;
                    }
                    if (!first) {
                        oss << ",";
                    }
                    first = false;
                    oss << "{";
                    oss << "\"time\":\"" << EscapeJson(r.time) << "\",";
                    oss << "\"chunk_id\":\"" << EscapeJson(r.chunk_id) << "\",";
                    oss << "\"owner\":\"" << EscapeJson(r.owner) << "\",";
                    oss << "\"success\":" << (r.success ? 1 : 0) << ",";
                    oss << "\"latency_ms\":" << r.latency_ms << ",";
                    oss << "\"strategy\":\"" << EscapeJson(r.strategy) << "\"";
                    oss << "}";
                    count++;
                }
                oss << "],";
                oss << "\"summary\":[";
                bool sfirst = true;
                for (const auto &it: agg) {
                    if (!sfirst) {
                        oss << ",";
                    }
                    sfirst = false;
                    const int total = it.second.total;
                    const int ok = it.second.success;
                    const long total_latency = it.second.latency_sum;
                    oss << "{";
                    oss << "\"owner\":\"" << EscapeJson(it.first) << "\",";
                    oss << "\"total\":" << total << ",";
                    oss << "\"success\":" << ok << ",";
                    oss << "\"latency_ms_sum\":" << total_latency << ",";
                    oss << "\"last_time\":\"" << EscapeJson(last_time[it.first]) << "\"";
                    oss << "}";
                }
                oss << "]";
                oss << "}";
                std::cout << oss.str() << "\n";
            } else {
                LogInfo("stats: download total_rows=" + std::to_string(rows.size()));
                int count = 0;
                for (const auto &r: rows) {
                    if (!owner_filter.empty() && r.owner != owner_filter) {
                        continue;
                    }
                    if (count >= limit) {
                        break;
                    }
                    LogInfo("stat: time=" + r.time + " owner=" + r.owner +
                            " success=" + std::to_string(r.success ? 1 : 0) +
                            " latency_ms=" + std::to_string(r.latency_ms) +
                            " strategy=" + r.strategy +
                            " chunk=" + r.chunk_id);
                    count++;
                }
            }
            return 0;
        }
        if (stats_type == "upload_meta") {
            std::vector<UploadMetaRow> rows;
            LoadUploadMeta(cfg.upload_meta_file, &rows);
            if (json_out) {
                std::ostringstream oss;
                oss << "{";
                oss << "\"type\":\"upload_meta\",";
                oss << "\"total_rows\":" << rows.size() << ",";
                oss << "\"items\":[";
                bool first = true;
                int count = 0;
                for (const auto &r: rows) {
                    if (count >= limit) {
                        break;
                    }
                    if (!first) {
                        oss << ",";
                    }
                    first = false;
                    oss << "{";
                    oss << "\"time\":\"" << EscapeJson(r.time) << "\",";
                    oss << "\"file_name\":\"" << EscapeJson(r.file_name) << "\",";
                    oss << "\"file_size\":\"" << EscapeJson(r.file_size) << "\",";
                    oss << "\"uploader\":\"" << EscapeJson(r.uploader) << "\",";
                    oss << "\"manifest\":\"" << EscapeJson(r.manifest) << "\"";
                    oss << "}";
                    count++;
                }
                oss << "]";
                oss << "}";
                std::cout << oss.str() << "\n";
            } else {
                LogInfo("stats: upload_meta total_rows=" + std::to_string(rows.size()));
            }
            return 0;
        }
        if (stats_type == "upload_replica") {
            std::vector<UploadReplicaRow> rows;
            LoadUploadReplica(cfg.upload_replica_file, &rows);
            if (json_out) {
                std::ostringstream oss;
                oss << "{";
                oss << "\"type\":\"upload_replica\",";
                oss << "\"total_rows\":" << rows.size() << ",";
                oss << "\"items\":[";
                bool first = true;
                int count = 0;
                for (const auto &r: rows) {
                    if (!owner_filter.empty() && r.target != owner_filter) {
                        continue;
                    }
                    if (count >= limit) {
                        break;
                    }
                    if (!first) {
                        oss << ",";
                    }
                    first = false;
                    oss << "{";
                    oss << "\"time\":\"" << EscapeJson(r.time) << "\",";
                    oss << "\"chunk_id\":\"" << EscapeJson(r.chunk_id) << "\",";
                    oss << "\"target\":\"" << EscapeJson(r.target) << "\",";
                    oss << "\"success\":" << (r.success ? 1 : 0);
                    oss << "}";
                    count++;
                }
                oss << "]";
                oss << "}";
                std::cout << oss.str() << "\n";
            } else {
                LogInfo("stats: upload_replica total_rows=" + std::to_string(rows.size()));
            }
            return 0;
        }
        LogWarn("stats: invalid --stats_type (download/upload_meta/upload_replica)");
        return 1;
    } else if (mode == "http") {
        //------/build/app --config "data/full_e2e/configs/node_1.yaml" --mode http --http_port 18888
        //可选展示
        const std::string http_port_arg = ParseArgValue(argc, argv, "--http_port");
        AppConfig cfg_http = cfg;
        if (!http_port_arg.empty()) {
            cfg_http.http_port = ParseIntOrDefault(http_port_arg, cfg_http.http_port);
        }
        RunHttpServer(cfg_http, &table, &dht_store);
        return 0;
    }

    LogInfo("app ready (MVP5)");
    return 0;
}

/*
 *好的！我按照你的演示步骤，逐一说明每个命令在 `main()` 函数中触发的逻辑流程：

---

## 🎬 演示流程与代码执行映射

### **步骤0：进入目录**
```bash
cd "/Users/zhangzhiqing/武大的/毕业设计/zzqGraduationProj/毕业设计正式项目/pro"
```
**代码执行**：无（这是shell操作）
---
### **步骤1：编译 + 生成输入文件**
```bash
cmake -S . -B build
cmake --build build
./build/mvp2_tests
printf "..." > "data/full_e2e/input.txt"
```

**代码执行**：
- `mvp2_tests` 是测试程序，不是 `main.cpp` 的主逻辑
- 其他都是编译和shell命令

---

### **步骤2：生成6节点配置**
```bash
# 生成 node_1.yaml ~ node_6.yaml
cat > "${CFG_DIR}/node_${i}.yaml" <<EOF
...
EOF
```

**代码执行**：无（这是生成配置文件，还没运行程序）

---

### **步骤3：启动 node2~node6 服务器**

#### 终端B~F分别执行：
```bash
./build/app --config "data/full_e2e/configs/node_2.yaml" --mode server
./build/app --config "data/full_e2e/configs/node_3.yaml" --mode server
...
```


**每个节点执行的 main() 逻辑**：

1. **读取配置** → 加载 `node_X.yaml`
2. **初始化日志** → 创建 `logs/full_node_X.log`
3. **解析命令行参数** → `--mode server`，但没有 `--upload`，所以 `chosen_input` 为空
4. **上传/校验分支** → 因为 `chosen_input` 为空，执行 `else` 分支：
   - 尝试加载 `chunk_index.tsv`（首次启动时文件不存在，跳过）
5. **初始化路由表** → 从配置的 `seed_nodes` 添加其他5个节点
6. **初始化DHT存储** → 尝试从本地索引加载分片（首次为空）
7. **进入 server 模式分支** → 启动TCP服务器，监听配置的端口（9862~9866）
8. **等待请求** → 阻塞在 `RunTcpServerOnceWithHandler()`，处理 incoming TCP 连接

**关键点**：这5个节点此时处于**待命状态**，等待其他节点来连接它们。

---

### **步骤4：node1 上传文件 + 分发副本**

```bash
./build/app --config "data/full_e2e/configs/node_1.yaml" \
  --upload "data/full_e2e/input.txt" --replica 5 \
  --manifest_out "data/full_e2e/manifest.txt"
```


**node1 执行的 main() 逻辑**：

1. **读取配置** → 加载 `node_1.yaml`
2. **初始化日志** → 创建 `logs/full_node_1.log`
3. **解析命令行参数**：
   - `upload_path = "data/full_e2e/input.txt"`
   - `replica_count = 5`
   - `manifest_out = "data/full_e2e/manifest.txt"`
   - `chosen_input = "data/full_e2e/input.txt"`（不为空）
4. **进入上传分支** → 因为 `chosen_input` 不为空，执行 `UploadAndReplicate()`：

   **UploadAndReplicate 内部流程**：
   - **分割文件** → 将 `input.txt` 切成多个分片（默认1MB一个）
   - **计算SHA256** → 为每个分片生成唯一ID
   - **保存分片** → 写入 `data/full_e2e/node_1/chunks/xxx.chunk`
   - **验证分片** → 重新计算哈希确保完整性
   - **保存索引** → 写入 `data/full_e2e/node_1/chunk_index.tsv`
   - **保存manifest** → 写入 `data/full_e2e/manifest.txt`（记录分片顺序）
   - **记录上传元数据** → 追加到 `upload_meta.tsv`

   - **分发副本**（因为 `replica_count = 5`）：
     - 遍历 `seed_nodes`（node2~node6）
     - 对每个目标节点，发送 `PUT_CHUNK_HEX2` 请求（通过TCP客户端）
     - 接收方（node2~node6的server）会保存分片并更新自己的索引
     - 记录副本结果到 `upload_replica.tsv`

   - **注意**：此时传入的 `table` 和 `dht_store` 是 `nullptr`，所以**不会更新DHT**

5. **初始化路由表** → 添加 seed_nodes
6. **初始化DHT存储** → 因为 `chosen_input` 不为空，执行 `StoreValueToDht()`：
   - 遍历刚上传的所有分片
   - 向所有 seed_nodes 发送 `STORE` 请求
   - 每个接收到 STORE 的节点会在内存中记录 `chunk_id → owners` 映射

7. **没有指定 --mode** → 不进入任何模式分支，程序直接退出

**关键点**：这一步是**一次性操作**，上传完成后程序就退出了，不会持续运行。

---

### **步骤5：启动 node1 服务器**

```bash
./build/app --config "data/full_e2e/configs/node_1.yaml" --mode server
```


**node1 执行的 main() 逻辑**：

1. **读取配置** → 加载 `node_1.yaml`
2. **初始化日志**
3. **解析命令行参数** → `--mode server`，没有 `--upload`，所以 `chosen_input` 为空
4. **上传/校验分支** → 因为 `chosen_input` 为空，执行 `else` 分支：
   - 加载 `chunk_index.tsv`（现在文件存在了，因为步骤4已上传）
   - 校验所有分片的完整性
5. **初始化路由表** → 添加 seed_nodes
6. **初始化DHT存储** → 从本地索引加载分片，填充 `dht_store`
7. **进入 server 模式分支** → 启动TCP服务器，监听端口 9861
8. **等待请求** → 阻塞，可以响应 FIND_CHUNK、GET_CHUNK 等请求

**关键点**：现在 node1 也进入了**待命状态**，和其他5个节点一样。

---

### **步骤6.1：验证副本落盘**

```bash
for i in 2 3 4; do
  awk -v id="${CHUNK_ID}" '$1==id {print "node_" n ": replica ok"; found=1} END{exit !found}' n="${i}" \
    "data/full_e2e/node_${i}/chunk_index.tsv"
done
```


**代码执行**：无（这是用 `awk` 直接读取TSV文件，不是运行C++程序）

**背后的逻辑**：步骤4中 node1 通过 `PUT_CHUNK_HEX2` 发送的分片，已经被 node2/3/4 的服务器接收并保存到各自的 `chunks` 目录，同时更新了 `chunk_index.tsv`。

---

### **步骤6.2：node5 明文下载单个分片**

```bash
./build/app --config "data/full_e2e/configs/node_5.yaml" --mode client \
  --peer "127.0.0.1:9862" --chunk_id "${CHUNK_ID}" --strategy round_robin
```


**node5 执行的 main() 逻辑**：

1. **读取配置** → 加载 `node_5.yaml`
2. **初始化日志**
3. **解析命令行参数**：
   - `mode = "client"`
   - `peer = "127.0.0.1:9862"`（node2的地址）
   - `chunk_id = "xxx..."`（要下载的分片ID）
   - `strategy = "round_robin"`
   - 没有 `--secure`，所以 `secure_mode = false`
4. **上传/校验分支** → `chosen_input` 为空，跳过
5. **初始化路由表** → 添加 seed_nodes
6. **初始化DHT存储** → 从本地索引加载（node5可能有步骤4收到的副本）
7. **进入 client 模式分支**：

   - 解析 peer 地址 → `host = "127.0.0.1"`, `port = 9862`
   - 因为指定了 `--chunk_id`，执行单分片下载逻辑：

     **DHT查找 owners**：
     - 调用 `IterativeFindValue(chunk_id, ...)`
     - 向路由表中的节点发送 `FIND_VALUE` 请求
     - 如果找到，返回 owners 列表（如 `["127.0.0.1:9861", "127.0.0.1:9862", ...]`）
     - 如果没找到，fallback 到直接向 peer（node2）发送 `FIND_CHUNK` 请求

     **应用下载策略**：
     - 调用 `ApplyDownloadStrategy("round_robin", &owners)`
     - 对 owners 列表进行轮询排序

     **下载分片**：
     - 调用 `DownloadChunkByOwners(owners, chunk_id, secure_mode=false, ...)`
     - 遍历 owners 列表，依次尝试：
       - 向 owner 发送 `GET_CHUNK xxx` 请求
       - 接收响应 `CHUNK_DATA <二进制数据>`
       - 计算收到数据的 SHA256，对比是否与 `chunk_id` 一致
       - 如果一致，保存数据并返回成功
       - 如果不一致或失败，尝试下一个 owner
     - 记录下载统计到 `download_stats.tsv`

8. **程序退出** → 下载完成后直接退出，不保存分片到磁盘（只是验证能下载）

**关键点**：这是**主动拉取**模式，node5 作为客户端去其他节点下载数据。

---

### **步骤6.3：node5 加密下载单个分片**

```bash
./build/app --config "data/full_e2e/configs/node_5.yaml" --mode client \
  --peer "127.0.0.1:9862" --chunk_id "${CHUNK_ID}" --strategy round_robin --secure
```


**与步骤6.2的区别**：

- 多了 `--secure` 标志 → `secure_mode = true`
- 在 client 模式中：
  - 加载 AES 和 HMAC 密钥（从配置文件）
  - 调用 `DownloadChunkByOwners(..., secure_mode=true, aes_key, hmac_key, ...)`
  - 发送的请求变成 `GET_CHUNK_SEC xxx`（而不是 `GET_CHUNK`）
  - 接收的响应是 `CHUNK_SEC <加密文本>`
  - 调用 `DecryptFromSecureText()` 解密
  - 验证解密后数据的 SHA256

**其他流程完全相同**。

---

### **步骤6.4：node6 按 manifest 下载完整文件**

```bash
./build/app --config "data/full_e2e/configs/node_6.yaml" --mode client \
  --peer "127.0.0.1:9861" \
  --manifest "data/full_e2e/manifest.txt" --out_file "data/full_e2e/output_6.bin" \
  --strategy round_robin
```


**node6 执行的 main() 逻辑**：

1. **读取配置** → 加载 `node_6.yaml`
2. **初始化日志**
3. **解析命令行参数**：
   - `mode = "client"`
   - `peer = "127.0.0.1:9861"`（node1的地址）
   - `manifest_in = "data/full_e2e/manifest.txt"`
   - `out_file = "data/full_e2e/output_6.bin"`
   - `strategy = "round_robin"`
4. **上传/校验分支** → 跳过
5. **初始化路由表** → 添加 seed_nodes
6. **初始化DHT存储** → 从本地索引加载
7. **进入 client 模式分支**：

   - 因为指定了 `--manifest`，执行 manifest 下载逻辑：

     **加载 manifest**：
     - 调用 `LoadChunkManifest("data/full_e2e/manifest.txt", &chunk_ids)`
     - 读取文件中的所有 chunk_id（每行一个）

     **创建输出文件**：
     - 打开 `data/full_e2e/output_6.bin`（二进制写入模式）

     **循环下载每个分片**：
     - 对 manifest 中的每个 `chunk_id`：
       1. 调用 `IterativeFindValue(chunk_id, ...)` 查找 owners
       2. 如果找不到，fallback 到向 peer 发送 `FIND_CHUNK`
       3. 应用 `round_robin` 策略排序 owners
       4. 调用 `DownloadChunkByOwners(...)` 下载分片数据
       5. 将下载的数据写入 `output_6.bin`（按顺序追加）

     **完成**：
     - 关闭输出文件
     - 日志记录 `"mvp5: file assembled: data/full_e2e/output_6.bin"`

8. **程序退出**

**关键点**：这是**完整文件恢复**流程，按顺序下载所有分片并组装成原始文件。

---

### **步骤6.5：一致性校验**

```bash
cmp "data/full_e2e/input.txt" "data/full_e2e/output_6.bin" && echo "一致性通过"
```


**代码执行**：无（这是 shell 的 `cmp` 命令，比较两个文件的字节是否完全相同）

**背后的逻辑**：步骤6.4下载的 `output_6.bin` 应该和原始上传的 `input.txt` 完全一致，证明整个 P2P 系统的上传→分发→下载流程正确无误。

---

### **步骤7：查询统计数据**

#### 7.1 查询下载统计
```bash
./build/app --config "data/full_e2e/configs/node_5.yaml" --mode stats \
  --stats_type download --json --limit 10 > "logs/full_stats_download.json"
```


**node5 执行的 main() 逻辑**：

1. **读取配置** → 加载 `node_5.yaml`
2. **初始化日志**
3. **解析命令行参数**：
   - `mode = "stats"`
   - `stats_type = "download"`
   - `--json` 标志 → `json_out = true`
   - `limit = 10`
4. **上传/校验分支** → 跳过
5. **初始化路由表** → 添加 seed_nodes
6. **初始化DHT存储** → 从本地索引加载
7. **进入 stats 模式分支**：

   - 因为 `stats_type == "download"`：
     - 调用 `LoadDownloadStats("data/full_e2e/node_5/download_stats.tsv", &rows)`
     - 解析 TSV 文件，每一行是一个下载记录（时间、chunk_id、owner、成功与否、延迟、策略）
     - 因为 `--json`，调用 `BuildDownloadStatsJson(rows, owner_filter="", limit=10)`
     - 生成 JSON 格式的统计报告（包含详细记录和汇总信息）
     - 输出到标准输出（被重定向到 `logs/full_stats_download.json`）

8. **程序退出**

---

#### 7.2 查询上传元数据
```bash
./build/app --config "data/full_e2e/configs/node_1.yaml" --mode stats \
  --stats_type upload_meta --json --limit 10 > "logs/full_stats_upload_meta.json"
```


**node1 执行的 main() 逻辑**：

- 类似步骤7.1，但 `stats_type == "upload_meta"`
- 调用 `LoadUploadMeta("data/full_e2e/node_1/upload_meta.tsv", &rows)`
- 生成上传文件的元数据统计（文件名、大小、上传者、manifest路径）
- 输出 JSON 到 `logs/full_stats_upload_meta.json`

---

#### 7.3 查询副本统计
```bash
./build/app --config "data/full_e2e/configs/node_1.yaml" --mode stats \
  --stats_type upload_replica --json --limit 20 > "logs/full_stats_upload_replica.json"
```


**node1 执行的 main() 逻辑**：

- 类似步骤7.1，但 `stats_type == "upload_replica"`
- 调用 `LoadUploadReplica("data/full_e2e/node_1/upload_replica.tsv", &rows)`
- 生成分片副本分发的统计（chunk_id、目标节点、成功与否）
- 输出 JSON 到 `logs/full_stats_upload_replica.json`

---

### **步骤8：HTTP 模式展示（可选）**

```bash
./build/app --config "data/full_e2e/configs/node_1.yaml" --mode http --http_port 18888
```


**node1 执行的 main() 逻辑**：

1. **读取配置** → 加载 `node_1.yaml`
2. **初始化日志**
3. **解析命令行参数**：
   - `mode = "http"`
   - `--http_port 18888` → 覆盖配置文件中的 HTTP 端口
4. **上传/校验分支** → 跳过
5. **初始化路由表** → 添加 seed_nodes
6. **初始化DHT存储** → 从本地索引加载
7. **进入 http 模式分支**：

   - 调用 `RunHttpServer(cfg_http, &table, &dht_store)`
   - 创建 HTTP 服务器，监听 `0.0.0.0:18888`
   - 等待 HTTP 请求，支持的端点：
     - `GET /status` → 返回节点状态（JSON）
     - `POST /upload?file=xxx&replica=5` → 触发上传
     - `POST /download?manifest=xxx&out=yyy&peer=zzz` → 触发下载
     - `GET /stats?type=download&limit=10` → 查询统计
   - 阻塞运行，直到用户按 Ctrl+C

8. **程序退出**（用户中断后）

---

### **步骤9：清理**

```bash
# 在所有终端按 Ctrl+C
```


**代码执行**：终止正在运行的服务器进程

---

## 📊 总结表格

| 步骤 | 命令类型 | 执行的 main() 分支 | 关键操作 |
|------|---------|-------------------|---------|
| **3** (B-F) | `--mode server` | `if (mode == "server")` | 启动TCP服务器，等待请求 |
| **4** | `--upload --replica 5` | `if (!chosen_input.empty())` | 分割文件→保存分片→分发副本→STORE到DHT |
| **5** (G) | `--mode server` | `if (mode == "server")` | 启动node1服务器 |
| **6.2** | `--mode client --chunk_id` | `else if (mode == "client")` | DHT查找→下载单个分片（明文） |
| **6.3** | `--mode client --chunk_id --secure` | `else if (mode == "client")` | DHT查找→下载单个分片（加密） |
| **6.4** | `--mode client --manifest` | `else if (mode == "client")` | 按manifest下载所有分片→组装文件 |
| **7.1** | `--mode stats --stats_type download` | `else if (mode == "stats")` | 读取下载统计TSV→输出JSON |
| **7.2** | `--mode stats --stats_type upload_meta` | `else if (mode == "stats")` | 读取上传元数据TSV→输出JSON |
| **7.3** | `--mode stats --stats_type upload_replica` | `else if (mode == "stats")` | 读取副本统计TSV→输出JSON |
| **8** | `--mode http` | `else if (mode == "http")` | 启动HTTP REST API服务器 |

---

## 🎯 核心要点

1. **同一个可执行文件 `app`**，通过不同的命令行参数进入完全不同的工作模式
2. **上传是一次性操作**，不需要 `--mode`，执行完就退出
3. **服务器是长期运行**，需要 `--mode server`，阻塞等待请求
4. **客户端是临时连接**，需要 `--mode client`，下载完就退出
5. **统计查询是只读操作**，需要 `--mode stats`，读取TSV文件后退出
6. **HTTP模式是另一种服务器**，需要 `--mode http`，提供REST API

这就是你的演示样例中每个命令对应的代码执行逻辑！😊
 *
 *
 *
 */
