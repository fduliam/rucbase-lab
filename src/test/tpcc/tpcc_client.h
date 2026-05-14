/* TPC-C 客户端 socket 封装：发 SQL，收响应。
 * 基于 RucBase 的简单文本协议（rmdb 接收以 \0 结尾的字符串，回写以 \0 结尾的结果）。
 *
 * 设计要点：
 *   - 单连接 = 单线程 = 一条事务通道（serializable 由服务端全局锁保证）
 *   - 提供：send_sql / parse_select_rows，便于上层 TPC-C 事务复用
 */

#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tpcc {

constexpr int RECV_BUF_SIZE = 8192;

class SqlClient {
public:
    SqlClient(const std::string &host, int port) {
        sockfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) throw std::runtime_error("socket failed");

        struct hostent *he = gethostbyname(host.c_str());
        if (!he) {
            ::close(sockfd_);
            throw std::runtime_error("gethostbyname failed for " + host);
        }
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr = *(struct in_addr*)he->h_addr;
        if (::connect(sockfd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(sockfd_);
            throw std::runtime_error("connect failed");
        }
    }

    ~SqlClient() { if (sockfd_ >= 0) ::close(sockfd_); }

    // 发送一条 SQL，返回 server 写回的字符串（含表格 / abort / failure 等）
    std::string send_sql(const std::string &sql) {
        if (::write(sockfd_, sql.c_str(), sql.size() + 1) <= 0) {
            throw std::runtime_error("send_sql write failed");
        }
        char buf[RECV_BUF_SIZE];
        std::string out;
        // 读直到收到 \0；rmdb 一次 write 一个完整响应
        ssize_t n = ::recv(sockfd_, buf, RECV_BUF_SIZE, 0);
        if (n <= 0) throw std::runtime_error("recv failed");
        // recv 内可能包含 \0 终止符
        size_t len = ::strnlen(buf, (size_t)n);
        out.assign(buf, len);
        return out;
    }

    // 判断结果是否为 abort / failure
    static bool is_abort(const std::string &resp) {
        return resp.find("abort") != std::string::npos;
    }
    static bool is_failure(const std::string &resp) {
        // RMDBError 的提示形式：以错误信息开头，含 "Error" 等关键词
        if (resp.find("failure") != std::string::npos) return true;
        if (resp.find("Error") != std::string::npos) return true;
        return false;
    }

    // 解析 RecordPrinter 输出的表格
    // 序列为：分隔行(+--) | 表头(|caption|) | 分隔行 | 数据行... | 分隔行 | "Total record(s)..."
    // 返回表头之后、最后一条分隔行之前的所有数据行
    static std::vector<std::vector<std::string>> parse_rows(const std::string &resp) {
        std::vector<std::vector<std::string>> rows;
        std::istringstream iss(resp);
        std::string line;
        // 状态机：等待表头(0) -> 遇到分隔行后跳过表头(1) -> 读数据行(2)
        int state = 0;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            if (line[0] == '+') {
                // 遇到分隔行
                if (state == 0) state = 1;       // 表头之前的顶分隔
                else if (state == 1) state = 2;  // 表头后面的分隔
                else if (state == 2) break;      // 数据后的底分隔
                continue;
            }
            if (line[0] != '|') continue;
            if (state == 1) continue;  // 这是表头行
            if (state != 2) continue;
            // 数据行拆分
            std::vector<std::string> cols;
            size_t pos = 1;
            while (pos < line.size()) {
                size_t nxt = line.find('|', pos);
                if (nxt == std::string::npos) break;
                std::string cell = line.substr(pos, nxt - pos);
                size_t a = cell.find_first_not_of(" \t");
                size_t b = cell.find_last_not_of(" \t");
                if (a == std::string::npos) cols.emplace_back("");
                else cols.emplace_back(cell.substr(a, b - a + 1));
                pos = nxt + 1;
            }
            if (!cols.empty()) rows.push_back(std::move(cols));
        }
        return rows;
    }

private:
    int sockfd_ = -1;
};

}  // namespace tpcc
