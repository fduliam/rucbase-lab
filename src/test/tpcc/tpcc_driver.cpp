/* TPC-C 性能驱动：起 N 个客户端并发跑 5 类事务，统计 tpmC 与 latency。
 *
 * 用法：
 *   ./tpcc_driver -h 127.0.0.1 -p 8765 -w 1 -t 4 -d 60
 *     -t 客户端线程数  -d 测试时长（秒）
 *
 * 实现策略（最粗糙基线）：
 *   - 所有写时算（update col=col±x）由"客户端 SELECT 后再 UPDATE"实现；
 *     由于服务端用全局大锁，单事务内无并发干扰。
 *   - 不实现 Order-Status / Stock-Level 中的 ORDER BY LIMIT 与 COUNT DISTINCT，
 *     改写为客户端扫描后处理。
 *   - 事务边界：BEGIN; ... ; COMMIT;（失败用 ABORT）
 */

#include "tpcc_client.h"
#include "tpcc_common.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <algorithm>

using namespace tpcc;

// =================== 事务统计 ===================
struct Stats {
    std::atomic<uint64_t> committed{0};
    std::atomic<uint64_t> aborted{0};
    std::atomic<uint64_t> latency_us_sum{0};
    std::vector<uint64_t> latencies_us;  // 仅采样部分
    std::mutex mu;

    void record(bool ok, uint64_t lat_us) {
        if (ok) committed.fetch_add(1, std::memory_order_relaxed);
        else aborted.fetch_add(1, std::memory_order_relaxed);
        latency_us_sum.fetch_add(lat_us, std::memory_order_relaxed);
        // 简单蓄水池：每 100 条记一条
        thread_local int cnt = 0;
        if ((cnt++ % 100) == 0) {
            std::lock_guard<std::mutex> g(mu);
            latencies_us.push_back(lat_us);
        }
    }
};

// =================== TPC-C 事务实现 ===================
class TpccWorker {
public:
    TpccWorker(SqlClient *cli, int warehouses, uint64_t seed,
               int n_items, int n_cust, int n_dist)
        : cli_(cli), W_(warehouses), n_items_(n_items),
          n_cust_(n_cust), n_dist_(n_dist), rg_(seed) {}

    // -------- New Order（简化版：不取 item.i_price，直接用固定单价，去掉 amount 计算的依赖）
    bool new_order() {
        int w_id = rg_.randint(1, W_);
        int d_id = rg_.randint(1, n_dist_);
        int c_id = rg_.randint(1, n_cust_);
        int ol_cnt = rg_.randint(5, 15);

        cli_->send_sql("BEGIN;");

        // 1. 取 district 的 d_next_o_id
        auto resp = cli_->send_sql(
            "SELECT d_next_o_id FROM district WHERE d_w_id=" + std::to_string(w_id) +
            " AND d_id=" + std::to_string(d_id) + ";");
        auto rows = SqlClient::parse_rows(resp);
        if (rows.empty()) { cli_->send_sql("ABORT;"); return false; }
        int o_id = std::atoi(rows[0][0].c_str());

        // 2. 自增 d_next_o_id（客户端算后写回）
        cli_->send_sql(
            "UPDATE district SET d_next_o_id=" + std::to_string(o_id + 1) +
            " WHERE d_w_id=" + std::to_string(w_id) + " AND d_id=" + std::to_string(d_id) + ";");

        // 3. 写 orders
        cli_->send_sql(
            "INSERT INTO orders VALUES (" +
            std::to_string(o_id) + "," + std::to_string(d_id) + "," +
            std::to_string(w_id) + "," + std::to_string(c_id) + ",0,0," +
            std::to_string(ol_cnt) + ",1);");

        // 4. 写 new_orders
        cli_->send_sql(
            "INSERT INTO new_orders VALUES (" +
            std::to_string(o_id) + "," + std::to_string(d_id) + "," + std::to_string(w_id) + ");");

        // 5. 每个 ol：取 stock，扣 quantity，写 order_line
        for (int ln = 1; ln <= ol_cnt; ++ln) {
            int i_id = rg_.randint(1, n_items_);
            // 取 stock.s_quantity
            auto r = cli_->send_sql(
                "SELECT s_quantity FROM stock WHERE s_w_id=" + std::to_string(w_id) +
                " AND s_i_id=" + std::to_string(i_id) + ";");
            auto rs = SqlClient::parse_rows(r);
            if (rs.empty()) { cli_->send_sql("ABORT;"); return false; }
            int qty = std::atoi(rs[0][0].c_str());
            int new_qty = (qty >= 10) ? qty - 5 : qty + 91;
            cli_->send_sql(
                "UPDATE stock SET s_quantity=" + std::to_string(new_qty) +
                " WHERE s_w_id=" + std::to_string(w_id) +
                " AND s_i_id=" + std::to_string(i_id) + ";");

            // 写 order_line（amount 用固定值 5.0 简化）
            cli_->send_sql(
                "INSERT INTO order_line VALUES (" +
                std::to_string(o_id) + "," + std::to_string(d_id) + "," +
                std::to_string(w_id) + "," + std::to_string(ln) + "," +
                std::to_string(i_id) + "," + std::to_string(w_id) + ",0,5,5.00,'ssssssssssssssssssssssss');");
        }

        cli_->send_sql("COMMIT;");
        return true;
    }

    // -------- Payment
    bool payment() {
        int w_id = rg_.randint(1, W_);
        int d_id = rg_.randint(1, n_dist_);
        int c_id = rg_.randint(1, n_cust_);
        float amt = rg_.randfloat(1.0f, 5000.0f);

        cli_->send_sql("BEGIN;");

        // 1. 读 warehouse.w_ytd
        auto rw = SqlClient::parse_rows(cli_->send_sql(
            "SELECT w_ytd FROM warehouse WHERE w_id=" + std::to_string(w_id) + ";"));
        if (rw.empty()) { cli_->send_sql("ABORT;"); return false; }
        float w_ytd = std::atof(rw[0][0].c_str());
        cli_->send_sql(
            "UPDATE warehouse SET w_ytd=" + std::to_string(w_ytd + amt) +
            " WHERE w_id=" + std::to_string(w_id) + ";");

        // 2. district.d_ytd
        auto rd = SqlClient::parse_rows(cli_->send_sql(
            "SELECT d_ytd FROM district WHERE d_w_id=" + std::to_string(w_id) +
            " AND d_id=" + std::to_string(d_id) + ";"));
        if (rd.empty()) { cli_->send_sql("ABORT;"); return false; }
        float d_ytd = std::atof(rd[0][0].c_str());
        cli_->send_sql(
            "UPDATE district SET d_ytd=" + std::to_string(d_ytd + amt) +
            " WHERE d_w_id=" + std::to_string(w_id) + " AND d_id=" + std::to_string(d_id) + ";");

        // 3. customer 余额：c_balance -=, c_ytd_payment +=, c_payment_cnt +=
        auto rc = SqlClient::parse_rows(cli_->send_sql(
            "SELECT c_balance, c_ytd_payment, c_payment_cnt FROM customer WHERE c_w_id=" +
            std::to_string(w_id) + " AND c_d_id=" + std::to_string(d_id) +
            " AND c_id=" + std::to_string(c_id) + ";"));
        if (rc.empty()) { cli_->send_sql("ABORT;"); return false; }
        float c_bal = std::atof(rc[0][0].c_str());
        float c_ytd = std::atof(rc[0][1].c_str());
        int   c_cnt = std::atoi(rc[0][2].c_str());
        cli_->send_sql(
            "UPDATE customer SET c_balance=" + std::to_string(c_bal - amt) +
            " WHERE c_w_id=" + std::to_string(w_id) +
            " AND c_d_id=" + std::to_string(d_id) +
            " AND c_id=" + std::to_string(c_id) + ";");
        cli_->send_sql(
            "UPDATE customer SET c_ytd_payment=" + std::to_string(c_ytd + amt) +
            " WHERE c_w_id=" + std::to_string(w_id) +
            " AND c_d_id=" + std::to_string(d_id) +
            " AND c_id=" + std::to_string(c_id) + ";");
        cli_->send_sql(
            "UPDATE customer SET c_payment_cnt=" + std::to_string(c_cnt + 1) +
            " WHERE c_w_id=" + std::to_string(w_id) +
            " AND c_d_id=" + std::to_string(d_id) +
            " AND c_id=" + std::to_string(c_id) + ";");

        // 4. 插入 history
        cli_->send_sql(
            "INSERT INTO history VALUES (" + std::to_string(c_id) + "," +
            std::to_string(d_id) + "," + std::to_string(w_id) + "," +
            std::to_string(d_id) + "," + std::to_string(w_id) + ",0," +
            std::to_string(amt) + ",'aaaaaaaaaaaaaaaaaaaaaaaa');");

        cli_->send_sql("COMMIT;");
        return true;
    }

    // -------- Order Status
    bool order_status() {
        int w_id = rg_.randint(1, W_);
        int d_id = rg_.randint(1, n_dist_);
        int c_id = rg_.randint(1, n_cust_);

        cli_->send_sql("BEGIN;");
        cli_->send_sql(
            "SELECT c_balance, c_first, c_last FROM customer WHERE c_w_id=" +
            std::to_string(w_id) + " AND c_d_id=" + std::to_string(d_id) +
            " AND c_id=" + std::to_string(c_id) + ";");
        // 读取该客户的所有订单（标准 spec 只要最新一笔，受限于 SQL 子集这里取所有再客户端选最大）
        auto rows = SqlClient::parse_rows(cli_->send_sql(
            "SELECT o_id, o_carrier_id, o_ol_cnt FROM orders WHERE o_w_id=" +
            std::to_string(w_id) + " AND o_d_id=" + std::to_string(d_id) + ";"));
        // 简化：不 filter c_id（缺等值索引就退化为全扫描），仅打印行数
        // 真正的 TPC-C 要 SELECT MAX(o_id) WHERE o_c_id = c_id；改进留给学生
        cli_->send_sql("COMMIT;");
        return true;
    }

    // -------- Delivery
    bool delivery() {
        int w_id = rg_.randint(1, W_);
        int carrier = rg_.randint(1, 10);
        cli_->send_sql("BEGIN;");
        for (int d_id = 1; d_id <= n_dist_; ++d_id) {
            // 取该区任意一个 new_order（实际 TPC-C 要最小 no_o_id；这里取扫描结果第一行）
            auto rs = SqlClient::parse_rows(cli_->send_sql(
                "SELECT no_o_id FROM new_orders WHERE no_w_id=" + std::to_string(w_id) +
                " AND no_d_id=" + std::to_string(d_id) + ";"));
            if (rs.empty()) continue;
            int no_o_id = std::atoi(rs[0][0].c_str());
            // 删 new_orders
            cli_->send_sql(
                "DELETE FROM new_orders WHERE no_w_id=" + std::to_string(w_id) +
                " AND no_d_id=" + std::to_string(d_id) +
                " AND no_o_id=" + std::to_string(no_o_id) + ";");
            // 更新 orders.o_carrier_id
            cli_->send_sql(
                "UPDATE orders SET o_carrier_id=" + std::to_string(carrier) +
                " WHERE o_w_id=" + std::to_string(w_id) +
                " AND o_d_id=" + std::to_string(d_id) +
                " AND o_id=" + std::to_string(no_o_id) + ";");
            // 更新 order_line.ol_delivery_d
            cli_->send_sql(
                "UPDATE order_line SET ol_delivery_d=1 WHERE ol_w_id=" + std::to_string(w_id) +
                " AND ol_d_id=" + std::to_string(d_id) +
                " AND ol_o_id=" + std::to_string(no_o_id) + ";");
        }
        cli_->send_sql("COMMIT;");
        return true;
    }

    // -------- Stock Level（简化版：扫描后客户端 count）
    bool stock_level() {
        int w_id = rg_.randint(1, W_);
        int d_id = rg_.randint(1, n_dist_);
        int threshold = rg_.randint(10, 20);
        cli_->send_sql("BEGIN;");
        // 读 d_next_o_id
        auto rd = SqlClient::parse_rows(cli_->send_sql(
            "SELECT d_next_o_id FROM district WHERE d_w_id=" + std::to_string(w_id) +
            " AND d_id=" + std::to_string(d_id) + ";"));
        if (rd.empty()) { cli_->send_sql("ABORT;"); return false; }
        // 简化：不做 ol_o_id 范围扫描（受限于 SQL），仅做一次 stock 扫描计数
        auto rs = SqlClient::parse_rows(cli_->send_sql(
            "SELECT s_quantity FROM stock WHERE s_w_id=" + std::to_string(w_id) + ";"));
        int low = 0;
        for (auto &row : rs) {
            if (std::atoi(row[0].c_str()) < threshold) ++low;
        }
        (void)low;
        cli_->send_sql("COMMIT;");
        return true;
    }

private:
    SqlClient *cli_;
    int W_;
    int n_items_, n_cust_, n_dist_;
    Rand rg_;
};

// =================== 主线程 ===================
int main(int argc, char *argv[]) {
    std::string host = "127.0.0.1";
    int port = 8765;
    int W = 1, threads = 1, duration = 30;
    int scale_div = 1;
    int opt;
    while ((opt = getopt(argc, argv, "h:p:w:t:d:S:")) != -1) {
        switch (opt) {
            case 'h': host = optarg; break;
            case 'p': port = std::atoi(optarg); break;
            case 'w': W = std::atoi(optarg); break;
            case 't': threads = std::atoi(optarg); break;
            case 'd': duration = std::atoi(optarg); break;
            case 'S': scale_div = std::max(1, std::atoi(optarg)); break;
            default: ;
        }
    }
    int n_items = MAXITEMS / scale_div;
    int n_cust  = CUST_PER_DIST / scale_div;
    int n_dist  = DIST_PER_WARE;  // 区不缩
    std::cout << "[driver] host=" << host << " port=" << port
              << " warehouses=" << W << " threads=" << threads
              << " duration=" << duration << "s scale=" << scale_div
              << " items=" << n_items << " cust/dist=" << n_cust << "\n";

    Stats stats;
    std::atomic<bool> stop{false};

    auto worker_main = [&](int tid) {
        try {
            SqlClient cli(host, port);
            TpccWorker w(&cli, W, 0xCAFE0u + tid * 7919u, n_items, n_cust, n_dist);
            Rand mix(0xBEEF0u + tid);
            while (!stop.load(std::memory_order_relaxed)) {
                int r = mix.randint(1, 100);
                auto t0 = std::chrono::steady_clock::now();
                bool ok = false;
                try {
                    if (r <= MIX_NEW_ORDER) ok = w.new_order();
                    else if (r <= MIX_NEW_ORDER + MIX_PAYMENT) ok = w.payment();
                    else if (r <= MIX_NEW_ORDER + MIX_PAYMENT + MIX_ORDER_STATUS) ok = w.order_status();
                    else if (r <= MIX_NEW_ORDER + MIX_PAYMENT + MIX_ORDER_STATUS + MIX_DELIVERY) ok = w.delivery();
                    else ok = w.stock_level();
                } catch (std::exception &e) {
                    std::cerr << "[t" << tid << "] exception: " << e.what() << "\n";
                    ok = false;
                }
                auto t1 = std::chrono::steady_clock::now();
                uint64_t lat = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                stats.record(ok, lat);
            }
        } catch (std::exception &e) {
            std::cerr << "[t" << tid << "] init failed: " << e.what() << "\n";
        }
    };

    auto t_start = std::chrono::steady_clock::now();
    std::vector<std::thread> ths;
    for (int i = 0; i < threads; ++i) ths.emplace_back(worker_main, i);

    std::this_thread::sleep_for(std::chrono::seconds(duration));
    stop.store(true);
    for (auto &t : ths) t.join();
    auto t_end = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t_end - t_start).count();

    uint64_t commit = stats.committed.load();
    uint64_t abort  = stats.aborted.load();
    uint64_t total  = commit + abort;
    double tps = commit / sec;

    std::cout << "\n=========== TPC-C Result ===========\n";
    std::cout << "duration:  " << sec << " s\n";
    std::cout << "committed: " << commit << "\n";
    std::cout << "aborted:   " << abort << "\n";
    std::cout << "TPS (commit/s): " << tps << "\n";
    std::cout << "tpmC (NewOrder/min approx): "
              << (tps * MIX_NEW_ORDER / 100.0) * 60.0 << "\n";
    if (!stats.latencies_us.empty()) {
        std::lock_guard<std::mutex> g(stats.mu);
        auto &v = stats.latencies_us;
        std::sort(v.begin(), v.end());
        auto pct = [&](double p) -> uint64_t {
            size_t i = (size_t)(p * (v.size() - 1));
            return v[i];
        };
        std::cout << "latency p50/p95/p99 (us): "
                  << pct(0.50) << " / " << pct(0.95) << " / " << pct(0.99) << "\n";
    }
    return 0;
}
