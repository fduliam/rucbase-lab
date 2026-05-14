/* TPC-C 常量与 schema 定义。
 * 适配 RucBase 的最小 SQL 子集：
 *   - 仅 INT / FLOAT / CHAR(n)
 *   - 无 DECIMAL，金额一律用 FLOAT
 *   - 无 DATE，时间一律用 INT（unix timestamp）
 *   - 无 PRIMARY KEY 语法，主键只通过 CREATE INDEX 显式创建
 *   - 无聚合函数：客户端用 SELECT * 后自行聚合
 *
 * 表使用率：
 *   - warehouse / district / customer / history / new_orders / orders / order_line / item / stock
 *   - 标准 TPC-C 9 张表
 *
 * 关键缩减（教学版）：
 *   - 字符串字段长度按 TPC-C spec 给的 max 建表；
 *   - 不区分 c_credit 'BC'/'GC'，简化 Payment；
 *   - new_orders/orders/order_line 用 (w_id,d_id,o_id) 索引；customer 用 (w_id,d_id,c_id)。
 */

#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace tpcc {

// ===== 标准 TPC-C 常量 =====
constexpr int DIST_PER_WARE = 10;          // 每仓 10 区
constexpr int CUST_PER_DIST = 3000;        // 每区 3000 客户
constexpr int MAXITEMS     = 100000;       // 商品总数
constexpr int ORD_PER_DIST = 3000;         // 初始订单数 == 客户数
constexpr int STOCK_PER_WARE = MAXITEMS;   // 每仓全量库存

// ===== 事务混合比例（标准 TPC-C） =====
constexpr int MIX_NEW_ORDER     = 45;
constexpr int MIX_PAYMENT       = 43;
constexpr int MIX_ORDER_STATUS  = 4;
constexpr int MIX_DELIVERY      = 4;
constexpr int MIX_STOCK_LEVEL   = 4;
// 总和 100

// ===== 建表 SQL（按依赖顺序）=====
// 注：RucBase 不支持 NOT NULL / PRIMARY KEY 语法，主键改为靠应用层与 CREATE INDEX 维护
inline std::vector<std::string> get_create_table_sqls() {
    return {
        // warehouse
        "CREATE TABLE warehouse ("
            "w_id INT, w_name CHAR(10), w_street_1 CHAR(20), w_street_2 CHAR(20), "
            "w_city CHAR(20), w_state CHAR(2), w_zip CHAR(9), w_tax FLOAT, w_ytd FLOAT"
        ");",
        // district
        "CREATE TABLE district ("
            "d_id INT, d_w_id INT, d_name CHAR(10), d_street_1 CHAR(20), d_street_2 CHAR(20), "
            "d_city CHAR(20), d_state CHAR(2), d_zip CHAR(9), d_tax FLOAT, d_ytd FLOAT, d_next_o_id INT"
        ");",
        // customer
        "CREATE TABLE customer ("
            "c_id INT, c_d_id INT, c_w_id INT, c_first CHAR(16), c_middle CHAR(2), c_last CHAR(16), "
            "c_street_1 CHAR(20), c_street_2 CHAR(20), c_city CHAR(20), c_state CHAR(2), c_zip CHAR(9), "
            "c_phone CHAR(16), c_since INT, c_credit CHAR(2), c_credit_lim FLOAT, c_discount FLOAT, "
            "c_balance FLOAT, c_ytd_payment FLOAT, c_payment_cnt INT, c_delivery_cnt INT, c_data CHAR(250)"
        ");",
        // history（无主键；本基线版本不做范围聚合，只做 insert）
        "CREATE TABLE history ("
            "h_c_id INT, h_c_d_id INT, h_c_w_id INT, h_d_id INT, h_w_id INT, "
            "h_date INT, h_amount FLOAT, h_data CHAR(24)"
        ");",
        // new_orders
        "CREATE TABLE new_orders ("
            "no_o_id INT, no_d_id INT, no_w_id INT"
        ");",
        // orders
        "CREATE TABLE orders ("
            "o_id INT, o_d_id INT, o_w_id INT, o_c_id INT, o_entry_d INT, "
            "o_carrier_id INT, o_ol_cnt INT, o_all_local INT"
        ");",
        // order_line
        "CREATE TABLE order_line ("
            "ol_o_id INT, ol_d_id INT, ol_w_id INT, ol_number INT, ol_i_id INT, "
            "ol_supply_w_id INT, ol_delivery_d INT, ol_quantity INT, ol_amount FLOAT, ol_dist_info CHAR(24)"
        ");",
        // item（只读表）
        "CREATE TABLE item ("
            "i_id INT, i_im_id INT, i_name CHAR(24), i_price FLOAT, i_data CHAR(50)"
        ");",
        // stock
        "CREATE TABLE stock ("
            "s_i_id INT, s_w_id INT, s_quantity INT, "
            "s_dist_01 CHAR(24), s_dist_02 CHAR(24), s_dist_03 CHAR(24), s_dist_04 CHAR(24), s_dist_05 CHAR(24), "
            "s_dist_06 CHAR(24), s_dist_07 CHAR(24), s_dist_08 CHAR(24), s_dist_09 CHAR(24), s_dist_10 CHAR(24), "
            "s_ytd FLOAT, s_order_cnt INT, s_remote_cnt INT, s_data CHAR(50)"
        ");",
    };
}

// ===== 建索引 SQL（在数据灌完后建，避免 insert 时反复维护索引）=====
inline std::vector<std::string> get_create_index_sqls() {
    return {
        "CREATE INDEX warehouse (w_id);",
        "CREATE INDEX district (d_w_id, d_id);",
        "CREATE INDEX customer (c_w_id, c_d_id, c_id);",
        "CREATE INDEX new_orders (no_w_id, no_d_id, no_o_id);",
        "CREATE INDEX orders (o_w_id, o_d_id, o_id);",
        "CREATE INDEX order_line (ol_w_id, ol_d_id, ol_o_id, ol_number);",
        "CREATE INDEX item (i_id);",
        "CREATE INDEX stock (s_w_id, s_i_id);",
    };
}

// ===== 随机数工具 =====
class Rand {
public:
    explicit Rand(uint64_t seed = 0xDEADBEEFULL) : rng_(seed) {}

    int randint(int lo, int hi) {  // [lo, hi]
        std::uniform_int_distribution<int> d(lo, hi);
        return d(rng_);
    }
    float randfloat(float lo, float hi) {
        std::uniform_real_distribution<float> d(lo, hi);
        return d(rng_);
    }
    // TPC-C non-uniform random
    int NURand(int A, int x, int y) {
        // C 是常数，这里取每次随机
        int C = randint(0, A);
        return ((((randint(0, A) | randint(x, y)) + C) % (y - x + 1)) + x);
    }
    std::string astring(int lo, int hi) {
        int n = randint(lo, hi);
        std::string s;
        s.reserve(n);
        for (int i = 0; i < n; ++i) s.push_back('a' + randint(0, 25));
        return s;
    }
    std::string nstring(int lo, int hi) {
        int n = randint(lo, hi);
        std::string s;
        s.reserve(n);
        for (int i = 0; i < n; ++i) s.push_back('0' + randint(0, 9));
        return s;
    }
    // TPC-C 标准的 last name 生成（C_LAST）
    std::string make_last(int num) {
        static const char *N[] = {"BAR","OUGHT","ABLE","PRI","PRES","ESE","ANTI","CALLY","ATION","EING"};
        std::string s;
        s += N[num / 100];
        s += N[(num / 10) % 10];
        s += N[num % 10];
        return s;
    }
private:
    std::mt19937_64 rng_;
};

// SQL 字符串字面量需要单引号包裹，转义内部单引号
inline std::string sql_quote(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out.push_back('\'');  // 简单处理：TPC-C 数据不会含单引号
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

}  // namespace tpcc
