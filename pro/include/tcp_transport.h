#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

// 启动 TCP 服务端，接收一条消息并返回响应
// - bind_host: 绑定地址（如 0.0.0.0）
// - port: 监听端口
// - response: 返回给客户端的消息内容
bool RunTcpServerOnce(const std::string& bind_host, int port,
                      const std::string& response);

// 启动 TCP 服务端，收到请求后由 handler 生成响应
// - handler: 入参为请求字符串，返回响应字符串
bool RunTcpServerOnceWithHandler(
    const std::string& bind_host, int port,
    const std::function<std::string(const std::string&)>& handler);

// 启动 TCP 客户端，发送一条消息并等待响应
// - host: 服务端地址
// - port: 服务端端口
// - message: 发送的消息内容
// - out_response: 接收到的响应
bool RunTcpClientOnce(const std::string& host, int port,
                      const std::string& message,
                      std::string* out_response);

// 启动 TCP 客户端，依次发送多条消息并接收响应
bool RunTcpClientSession(const std::string& host, int port,
                         const std::vector<std::string>& messages,
                         std::vector<std::string>* out_responses);
