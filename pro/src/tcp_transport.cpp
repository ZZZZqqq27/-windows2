#include "tcp_transport.h"

#include "logger.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace {
    bool WriteAll(int fd, const void *data, std::size_t len) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        std::size_t left = len;
        while (left > 0) {
            const ssize_t n = ::send(fd, p, left, 0);
            if (n <= 0) {
                return false;
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        return true;
    }

    bool ReadAll(int fd, void *data, std::size_t len) {
        uint8_t *p = static_cast<uint8_t *>(data);
        std::size_t left = len;
        while (left > 0) {
            const ssize_t n = ::recv(fd, p, left, 0);
            if (n <= 0) {
                return false;
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        return true;
    }

    bool SendMessage(int fd, const std::string &msg) {
        const uint32_t len = static_cast<uint32_t>(msg.size());
        const uint32_t be_len = htonl(len);
        if (!WriteAll(fd, &be_len, sizeof(be_len))) {
            return false;
        }
        if (!msg.empty()) {
            return WriteAll(fd, msg.data(), msg.size());
        }
        return true;
    }

    bool ReceiveMessage(int fd, std::string *out) {
        if (!out) {
            return false;
        }
        uint32_t be_len = 0;
        if (!ReadAll(fd, &be_len, sizeof(be_len))) {
            return false;
        }
        const uint32_t len = ntohl(be_len);
        out->assign(len, '\0');
        if (len == 0) {
            return true;
        }
        return ReadAll(fd, &(*out)[0], len);
    }
} // namespace
//------对外的api
bool RunTcpServerOnce(const std::string &bind_host, int port,
                      const std::string &response) {
    return RunTcpServerOnceWithHandler(
        bind_host, port, [response](const std::string &) { return response; });
}

//------对外的api，服务端的方法
bool RunTcpServerOnceWithHandler(
    const std::string &bind_host, int port,
    const std::function<std::string(const std::string &)> &handler) {
    LogInfo("tcp server start: host=" + bind_host + " port=" + std::to_string(port));
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LogError("tcp server socket failed: " + std::string(std::strerror(errno)));
        return false;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, bind_host.c_str(), &addr.sin_addr) != 1) {
        LogError("tcp server invalid bind host");
        ::close(server_fd);
        return false;
    }

    if (::bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        LogError("tcp server bind failed: " + std::string(std::strerror(errno)));
        ::close(server_fd);
        return false;
    }

    if (::listen(server_fd, 1) < 0) {
        LogError("tcp server listen failed: " + std::string(std::strerror(errno)));
        ::close(server_fd);
        return false;
    }

    // 允许多次连接（顺序处理）
    const int max_clients = 1024;
    for (int handled = 0; handled < max_clients; ++handled) {
        LogInfo("tcp server waiting for client...");
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd =
                ::accept(server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
        if (client_fd < 0) {
            LogError("tcp server accept failed: " + std::string(std::strerror(errno)));
            ::close(server_fd);
            return false;
        }

        while (true) {
            std::string message;
            if (!ReceiveMessage(client_fd, &message)) {
                break;
            }
            LogInfo("tcp server received: " + message);

            const std::string response = handler(message);
            if (!SendMessage(client_fd, response)) {
                LogError("tcp server send failed");
                ::close(client_fd);
                ::close(server_fd);
                return false;
            }
            LogInfo("tcp server replied: " + response);
        }
        ::close(client_fd);
    }

    ::close(server_fd);
    LogInfo("tcp server done");
    return true;
}

//------对外的api，跟下面那个没区别，也是客户端
bool RunTcpClientOnce(const std::string &host,
    int port,
                      const std::string &message,
                      std::string *out_response) {
    LogInfo("tcp client start: host=" + host + " port=" + std::to_string(port));
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LogError("tcp client socket failed: " + std::string(std::strerror(errno)));
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        LogError("tcp client invalid host");
        ::close(fd);
        return false;
    }

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        LogError("tcp client connect failed: " + std::string(std::strerror(errno)));
        ::close(fd);
        return false;
    }

    if (!SendMessage(fd, message)) {
        LogError("tcp client send failed");
        ::close(fd);
        return false;
    }
    LogInfo("tcp client sent: " + message);

    std::string response;
    if (!ReceiveMessage(fd, &response)) {
        LogError("tcp client receive failed");
        ::close(fd);
        return false;
    }
    LogInfo("tcp client received: " + response);

    if (out_response) {
        *out_response = response;
    }

    ::close(fd);
    LogInfo("tcp client done");
    return true;
}

//------对外的api
//重要方法 ，客户端发tcp消息的核心方法
bool RunTcpClientSession(const std::string &host, // 目标节点IP
                         int port,// 目标节点端口
                         const std::vector<std::string> &messages,// 要发的命令（比如 FIND_NODE xxx）
                         std::vector<std::string> *out_responses) { // 输出：对方返回的结果
    LogInfo("tcp client session start: host=" + host + " port=" + std::to_string(port));
    // 创建socket：AF_INET(IPv4) + SOCK_STREAM(TCP) + 0(默认协议)
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LogError("tcp client socket failed: " + std::string(std::strerror(errno)));
        return false;
    }
    //------------
//2. 配置目标节点的地址（IP + 端口）
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    // 把字符串IP(192.168.x.x)转成二进制网络地址
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        LogError("tcp client invalid host");
        ::close(fd);
        return false;
    }
    // connect 系统调用：主动连接远程节点
    // 底层自动完成 TCP 三次握手！
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        LogError("tcp client connect failed: " + std::string(std::strerror(errno)));
        ::close(fd);
        return false;
    }

    if (out_responses) {
        out_responses->clear();
    }

    for (const auto &msg: messages) {
        if (!SendMessage(fd, msg)) {
            LogError("tcp client send failed");
            ::close(fd);
            return false;
        }
        LogInfo("tcp client sent: " + msg);

        std::string resp;
        if (!ReceiveMessage(fd, &resp)) {
            LogError("tcp client receive failed");
            ::close(fd);
            return false;
        }
        //把从内核态的 resp
        LogInfo("tcp client received: " + resp);
        if (out_responses) {
            out_responses->push_back(resp);
        }
    }

    ::close(fd);
    LogInfo("tcp client session done");
    return true;
}
