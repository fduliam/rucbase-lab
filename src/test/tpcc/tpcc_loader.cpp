/* TPC-C 数据加载器：通过 socket 客户端连接 rmdb，建表→灌数据→建索引。
 *
 * 用法：
 *   ./tpcc_loader -h 127.0.0.1 -p 8765 -w <warehouses>
 *
 * 加载顺序：
 *   1. CREATE TABLE × 9（无索引）
 *   2. INSERT item × 100000
 *   3. for w in 1..W:
 *        INSERT warehouse
 *        INSERT stock × 100000
 *        for d in 1..10:
 *          INSERT district
 *          INSERT customer × 3000 + history × 3000
 *          INSERT orders × 3000 + order_line × ~30000 + new_orders × ~900
 *   4. CREATE INDEX × 8
 *
 * 注意：粗糙基线版每条 SQL 都会被服务端串行执行（全局大锁），灌数据较慢。
 *      即使 W=1 也要约 100k+30k+30k+90k+30k+...次 INSERT，需要数分钟。
 *      可通过命令行 --skip-data 只建表/索引用于调试。
 */

#include "tpcc_client.h"
#include "tpcc_common.h"

#include <chrono>
#include <cstdio>
#include <getopt.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

using namespace tpcc;

static std::string F(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}
static std::string I(int v) { return std::to_string(v); }

int main(int argc, char *argv[]) {
    std::string host = "127.0.0.1";
    int port = 8765;
    int W = 1;
    bool skip_data = false;
    bool drop_first = false;
    // 缩放因子：实验室环境下全量 TPC-C 太慢，可用 --scale=10 表示所有表行数 / 10
    int scale_div = 1;

    static struct option long_opts[] = {
        {"host", required_argument, nullptr, 'h'},
        {"port", required_argument, nullptr, 'p'},
        {"warehouses", required_argument, nullptr, 'w'},
        {"skip-data", no_argument, nullptr, 's'},
        {"drop", no_argument, nullptr, 'd'},
        {"scale", required_argument, nullptr, 'S'},
        {nullptr, 0, nullptr, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:w:sdS:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'h': host = optarg; break;
            case 'p': port = std::atoi(optarg); break;
            case 'w': W = std::atoi(optarg); break;
            case 's': skip_data = true; break;
            case 'd': drop_first = true; break;
            case 'S': scale_div = std::max(1, std::atoi(optarg)); break;
            default: ;
        }
    }
    int n_items = MAXITEMS / scale_div;
    int n_cust  = CUST_PER_DIST / scale_div;
    int n_ord   = ORD_PER_DIST / scale_div;
    int n_stock = STOCK_PER_WARE / scale_div;
    int new_order_cutoff = std::max(1, (int)((double)2101 / scale_div));
    std::cout << "[loader] host=" << host << " port=" << port
              << " warehouses=" << W << " scale_div=" << scale_div
              << " items=" << n_items << " cust/dist=" << n_cust
              << " ord/dist=" << n_ord << " stock/ware=" << n_stock << "\n";

    SqlClient client(host, port);

    auto exec = [&](const std::string &sql) {
        auto resp = client.send_sql(sql);
        if (SqlClient::is_failure(resp) || SqlClient::is_abort(resp)) {
            std::cerr << "[loader] SQL FAILED: " << sql.substr(0, 120) << "...\n  -> " << resp << "\n";
        }
    };

    // 0. 可选：先 drop（开发期反复重灌用）
    if (drop_first) {
        const char *tabs[] = {"warehouse","district","customer","history",
                              "new_orders","orders","order_line","item","stock"};
        for (auto t : tabs) {
            try { exec(std::string("DROP TABLE ") + t + ";"); } catch (...) {}
        }
    }

    // 1. 建表（不带索引，加快插入）
    std::cout << "[loader] creating tables...\n";
    for (auto &sql : get_create_table_sqls()) exec(sql);

    if (skip_data) {
        std::cout << "[loader] --skip-data set, creating indexes only.\n";
        for (auto &sql : get_create_index_sqls()) exec(sql);
        return 0;
    }

    Rand rg(0xC0FFEE);
    auto t_start = std::chrono::steady_clock::now();

    // 2. ITEM 表
    std::cout << "[loader] loading item ...\n";
    for (int i = 1; i <= n_items; ++i) {
        std::ostringstream os;
        os << "INSERT INTO item VALUES ("
           << I(i) << ", "
           << I(rg.randint(1, 10000)) << ", "
           << sql_quote(rg.astring(14, 24)) << ", "
           << F(rg.randfloat(1.0f, 100.0f)) << ", "
           << sql_quote(rg.astring(26, 50))
           << ");";
        exec(os.str());
        if (i % 5000 == 0) std::cout << "  item " << i << "/" << n_items << "\n";
    }

    // 3. 各仓 + 区 + 客户 + 订单
    for (int w = 1; w <= W; ++w) {
        std::cout << "[loader] loading warehouse " << w << " ...\n";
        // warehouse
        {
            std::ostringstream os;
            os << "INSERT INTO warehouse VALUES ("
               << I(w) << ", " << sql_quote(rg.astring(6, 10)) << ", "
               << sql_quote(rg.astring(10, 20)) << ", " << sql_quote(rg.astring(10, 20)) << ", "
               << sql_quote(rg.astring(10, 20)) << ", " << sql_quote(rg.astring(2, 2)) << ", "
               << sql_quote(rg.nstring(9, 9)) << ", "
               << F(rg.randfloat(0.0f, 0.2f)) << ", "
               << F(300000.0f)
               << ");";
            exec(os.str());
        }

        // stock
        std::cout << "  loading stock for w=" << w << " ...\n";
        for (int i = 1; i <= n_stock; ++i) {
            std::ostringstream os;
            os << "INSERT INTO stock VALUES ("
               << I(i) << ", " << I(w) << ", " << I(rg.randint(10, 100)) << ", "
               << sql_quote(rg.astring(24, 24)) << ", " << sql_quote(rg.astring(24, 24)) << ", "
               << sql_quote(rg.astring(24, 24)) << ", " << sql_quote(rg.astring(24, 24)) << ", "
               << sql_quote(rg.astring(24, 24)) << ", " << sql_quote(rg.astring(24, 24)) << ", "
               << sql_quote(rg.astring(24, 24)) << ", " << sql_quote(rg.astring(24, 24)) << ", "
               << sql_quote(rg.astring(24, 24)) << ", " << sql_quote(rg.astring(24, 24)) << ", "
               << F(0.0f) << ", " << I(0) << ", " << I(0) << ", "
               << sql_quote(rg.astring(26, 50))
               << ");";
            exec(os.str());
            if (i % 10000 == 0) std::cout << "    stock " << i << "/" << n_stock << "\n";
        }

        // 每个区
        for (int d = 1; d <= DIST_PER_WARE; ++d) {
            // district
            {
                std::ostringstream os;
                os << "INSERT INTO district VALUES ("
                   << I(d) << ", " << I(w) << ", " << sql_quote(rg.astring(6, 10)) << ", "
                   << sql_quote(rg.astring(10, 20)) << ", " << sql_quote(rg.astring(10, 20)) << ", "
                   << sql_quote(rg.astring(10, 20)) << ", " << sql_quote(rg.astring(2, 2)) << ", "
                   << sql_quote(rg.nstring(9, 9)) << ", "
                   << F(rg.randfloat(0.0f, 0.2f)) << ", "
                   << F(30000.0f) << ", "
                   << I(3001)  // d_next_o_id 初始为 3001
                   << ");";
                exec(os.str());
            }

            // customer + history
            for (int c = 1; c <= n_cust; ++c) {
                std::string clast = (c <= 1000) ? rg.make_last(c - 1)
                                                : rg.make_last(rg.NURand(255, 0, 999));
                std::ostringstream os;
                os << "INSERT INTO customer VALUES ("
                   << I(c) << ", " << I(d) << ", " << I(w) << ", "
                   << sql_quote(rg.astring(8, 16)) << ", "
                   << sql_quote("OE") << ", "
                   << sql_quote(clast) << ", "
                   << sql_quote(rg.astring(10, 20)) << ", " << sql_quote(rg.astring(10, 20)) << ", "
                   << sql_quote(rg.astring(10, 20)) << ", " << sql_quote(rg.astring(2, 2)) << ", "
                   << sql_quote(rg.nstring(9, 9)) << ", "
                   << sql_quote(rg.nstring(16, 16)) << ", "
                   << I(0) << ", "
                   << sql_quote("GC") << ", "
                   << F(50000.0f) << ", "
                   << F(rg.randfloat(0.0f, 0.5f)) << ", "
                   << F(-10.0f) << ", "
                   << F(10.0f) << ", "
                   << I(1) << ", " << I(0) << ", "
                   << sql_quote(rg.astring(50, 250))
                   << ");";
                exec(os.str());

                std::ostringstream osh;
                osh << "INSERT INTO history VALUES ("
                    << I(c) << ", " << I(d) << ", " << I(w) << ", "
                    << I(d) << ", " << I(w) << ", "
                    << I(0) << ", " << F(10.0f) << ", "
                    << sql_quote(rg.astring(12, 24))
                    << ");";
                exec(osh.str());
            }
            std::cout << "    district " << d << " customers loaded\n";

            // orders + order_line + new_orders
            // 使用随机 c_id 排列
            std::vector<int> cids(n_cust);
            for (int i = 0; i < n_cust; ++i) cids[i] = i + 1;
            // shuffle
            for (int i = n_cust - 1; i > 0; --i) {
                int j = rg.randint(0, i);
                std::swap(cids[i], cids[j]);
            }
            for (int o = 1; o <= n_ord; ++o) {
                int c_id = cids[(o - 1) % n_cust];
                int ol_cnt = rg.randint(5, 15);
                int carrier = (o < new_order_cutoff) ? rg.randint(1, 10) : 0;
                {
                    std::ostringstream os;
                    os << "INSERT INTO orders VALUES ("
                       << I(o) << ", " << I(d) << ", " << I(w) << ", " << I(c_id) << ", "
                       << I(0) << ", "
                       << I(carrier) << ", " << I(ol_cnt) << ", " << I(1)
                       << ");";
                    exec(os.str());
                }
                if (o >= new_order_cutoff) {
                    std::ostringstream os;
                    os << "INSERT INTO new_orders VALUES ("
                       << I(o) << ", " << I(d) << ", " << I(w) << ");";
                    exec(os.str());
                }
                for (int ln = 1; ln <= ol_cnt; ++ln) {
                    std::ostringstream os;
                    os << "INSERT INTO order_line VALUES ("
                       << I(o) << ", " << I(d) << ", " << I(w) << ", " << I(ln) << ", "
                       << I(rg.randint(1, n_items)) << ", " << I(w) << ", "
                       << I(0) << ", "
                       << I(5) << ", "
                       << F((o < new_order_cutoff) ? 0.0f : rg.randfloat(0.01f, 9999.99f)) << ", "
                       << sql_quote(rg.astring(24, 24))
                       << ");";
                    exec(os.str());
                }
                if (o % 500 == 0) std::cout << "    orders " << o << "/" << n_ord << "\n";
            }
        }
    }

    // 4. 建索引
    std::cout << "[loader] creating indexes ...\n";
    for (auto &sql : get_create_index_sqls()) exec(sql);

    auto t_end = std::chrono::steady_clock::now();
    auto sec = std::chrono::duration<double>(t_end - t_start).count();
    std::cout << "[loader] DONE in " << sec << " seconds.\n";
    return 0;
}
