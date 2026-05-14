
# Bonus实验：事务系统性能优化挑战


> **本实验在原[RucBase](https://github.com/ruc-deke/rucbase-lab)框架上扩展完成**，提供：
> 1. 一版**最粗糙、可正确运行**的事务系统
> 2. 一套完整的 **TPC-C benchmark 工具链**（loader + 5 类事务 driver + tpmC 统计）
> 3. **故意保留**的性能瓶颈，作为本实验的优化空间
>
> 本实验的目标**不是从零造数据库**，而是在一版"能跑但很慢"的事务系统之上，**像真实数据库工程师一样定位瓶颈、提出优化、量化收益**。

---

## 目录

1. [项目背景与目标](#1-项目背景与目标)
2. [代码结构与改动概览](#2-代码结构与改动概览)
3. [部署与运行指导](#3-部署与运行指导)
4. [基线性能数据](#4-基线性能数据)
5. [参考优化方向](#5-参考优化方向)
6. [常见问题](#6-常见问题)

---

## 1. 项目背景与目标

### 1.1 RucBase 简介

RucBase 是中国人民大学数据库实验课的教学数据库框架，覆盖了一个最小化关系数据库的核心模块：存储引擎（buffer pool / record manager）、索引（B+ 树）、查询执行（火山模型）、SQL 解析、事务与并发控制、日志恢复。学生在 4 个 Lab 中分别完成存储管理、索引管理、查询执行、并发控制四大模块。

### 1.2 本实验的定位



本实验在保留原框架的前提下，**最少侵入地补齐了一版可运行的事务系统**，并且**故意保留了大量性能问题**：

| 设计选择 | 教学意图 |
|---|---|
| 表级 S/X 锁 + wait-die 死锁预防（粒度粗，写事务之间互斥） | 让学生看到"正确性 vs 并发粒度"的权衡，进一步可下沉到行级锁 |
| 写操作通过 `write_set` 物理回滚 | 让学生理解 undo 的最朴素形式（与 ARIES 对比） |
| 强制走 `SeqScan`，不依赖 B+ 树 | 让学生在做完 Lab2 后切换到 IndexScan，亲眼看到 100× 提速 |


### 1.3 当前锁机制：表级 S/X 锁 + wait-die

为了让事务在多线程下既正确又不至于死锁卡死，本实验实现了一版**最小可行的两阶段锁**，关键事实如下：

- **粒度**：表级。每张表维护一个共享/排他锁请求队列（`LockManager::lock_table_`，键为 `(txn_id, tab_fd)`）。
- **模式**：S（共享，读）/ X（排他，写）。`SeqScanExecutor` 在 `beginTuple` 时申请 S 锁，`Insert/Update/Delete` 在执行前申请 X 锁；同一事务对同一张表升级 S→X 也走同一个队列。
- **释放时机**：严格 2PL —— 锁全部累计到事务结束（`commit` 或 `abort`）时由 `TransactionManager` 一次性释放，事务执行期间只加锁、不释放。
- **死锁预防**：**wait-die**（见下文）。**没有**死锁检测线程，也没有超时回滚。

> 关键代码：`src/transaction/concurrency/lock_manager.{h,cpp}`、`src/transaction/transaction_manager.cpp`。

#### 1.3.1 wait-die 是什么

wait-die 是经典的**死锁预防**算法（Rosenkrantz, 1978），核心是用事务的开始时间戳 `start_ts` 给所有事务排个全局年龄序，**老的可以等，年轻的必须死**：

> 当事务 T 申请的锁与持锁者 H 不兼容时：
> - 若 T 比 H **老**（`T.start_ts < H.start_ts`）→ T **wait**，进队列继续等
> - 若 T 比 H **年轻**（`T.start_ts > H.start_ts`）→ T **die**，立即 abort 并由 driver 重试

因为"被迫等待"这条边只会从老指向年轻，wait-for 图里**永远不会形成环**，从根本上杜绝死锁。代价是**年轻事务会被反复 abort**——这正是基线测试中 abort 率高达 85–89% 的根因（见 4.1）。

---

## 2. benchmark概览

### 2.0 TPC-C 简介

[TPC-C](https://www.tpc.org/tpcc/) 是事务处理性能委员会（Transaction Processing Performance Council）于 1992 年发布、至今仍是衡量 OLTP 数据库性能的**事实标准** benchmark。它模拟一个批发供应商的订单处理业务，包含 9 张表（warehouse / district / customer / history / orders / new_orders / order_line / item / stock）和 5 类事务，按规范权重混合执行：

| 事务 | 权重 | 业务语义 | 读写特征 |
|---|---|---|---|
| **NewOrder** | 45% | 接收一笔新订单（含 5–15 个 item） | 读重 + 写中等，跨多表 |
| **Payment** | 43% | 客户付款，更新仓库 / 区 / 客户余额 | 写重，热点在 warehouse / district |
| **OrderStatus** | 4% | 查询客户最近一笔订单状态 | 只读 |
| **Delivery** | 4% | 后台批量发货（一次处理 10 笔订单） | 写重 |
| **StockLevel** | 4% | 查询近期低库存商品数 | 只读，扫描较多 |

**核心特征**：

- **写密集**：约 92% 的事务包含写操作，是典型的 OLTP 工作负载，能放大并发控制、日志、锁等模块的瓶颈。
- **数据规模可调**：以 `warehouse` 数 W 为基准，每个仓库约 100 MB 数据；其他表行数与 W 成正比，因此 benchmark 既可在单机小规模验证，也可在分布式集群上压测。
- **核心指标 tpmC**：每分钟成功提交的 NewOrder 事务数，**只统计 NewOrder**（其他事务作为背景负载）。tpmC 越高越好。
- **强一致性约束**：规范要求在跑分结束后做一致性检查（如 `D_NEXT_O_ID = max(O_ID) + 1`），任何并发控制 bug 都会被暴露。

**为什么选它**：TPC-C 既能压力测试**正确性**（5 类事务跨表更新，没有正确的并发控制必然破坏一致性），又能压力测试**性能**（读写混合 + 热点行 + 大表扫描），是评估事务系统优化效果的最佳标尺。

### 2.1 TPC-C 工具链文件

```
src/test/tpcc/
├── CMakeLists.txt          # 编译两个可执行文件：tpcc_loader / tpcc_driver
├── tpcc_common.h           # 9 张表的 schema、TPC-C 常量、随机数据生成
├── tpcc_client.h           # 简单的 socket SQL 客户端封装
├── tpcc_loader.cpp         # 灌数据（支持 --scale 缩放）
└── tpcc_driver.cpp         # 5 类事务的并发驱动 + tpmC/latency 统计
```

### 2.2 TPC-C 5 类事务实现

| 事务 | 在 driver 中的简化 |
|---|---|
| **NewOrder**（45%） | 标准实现，扣 stock 库存、写 orders/new_orders/order_line |
| **Payment**（43%） | 标准实现，更新 warehouse/district/customer 余额，写 history |
| **OrderStatus**（4%） | 简化：未实现 `MAX(o_id)`，由客户端扫描代替 |
| **Delivery**（4%） | 简化：未实现 `MIN(no_o_id)`，取扫描结果第一行 |
| **StockLevel**（4%） | 简化：用客户端 COUNT 代替 `COUNT(DISTINCT)` |

> 这 3 处简化都是因为原 RucBase 不支持聚合函数（`MIN/MAX/COUNT(DISTINCT)`）。**实现聚合算子也是一个可选的优化方向**，见第 6 节。

---

## 3. 部署与运行指导

### 3.1 环境要求

- Linux（已在 CentOS 7 / Ubuntu 20.04 上验证）
- CMake ≥ 3.13
- GCC ≥ 7.3 / Clang ≥ 6.0（C++17）
- flex / bison（构建解析器）
- 磁盘空闲 ≥ 5 GB（scale=1 全量灌库 + golden 备份各占 2.3 GB）

**可以直接跳到[3.6节](##3.6-三脚本一键复现)，使用脚本完成编译/数据准备/baseline跑测**

### 3.2 编译

```bash
cd /path/to/rucbase-lab
cmake -B build
cmake --build build --target rmdb tpcc_loader tpcc_driver -j$(nproc)
```

编译产物：

```
build/bin/rmdb           # 数据库 server，端口硬编码 8765
build/bin/tpcc_loader    # TPC-C 数据灌入工具
build/bin/tpcc_driver    # TPC-C 性能驱动 + tpmC/latency 统计
```

### 3.3 命令参数说明（先看清，后面脚本才好读）

#### 3.3.1 `rmdb`（server）

```bash
rmdb <database_name>
```

| 关键事实 | 说明 |
|---|---|
| **唯一参数**就是数据库名 | 没有 `-h` / `-p`；端口**硬编码 8765**，监听 `0.0.0.0` |
| 数据库目录 = `<cwd>/<database_name>/` | 在哪个目录起 server，库就建在哪个目录下；目录不存在会自动 `create_db` |
| 启动后会自动 `analyze → redo → undo` 做 recovery | 上次未优雅关闭时，会从 WAL 恢复 |
| **关闭只能用 `kill -INT`**（SIGINT） | server 注册了 SIGINT handler，会优雅 `close_db()` 落盘所有脏页 |

#### 3.3.2 `tpcc_loader`

```
tpcc_loader [-h <host>] [-p <port>] [-w <W>] [-S <scale>|--scale <scale>] [-s|--skip-data] [-d|--drop]
```

| 短选项 | 长选项 | 含义 | 默认 |
|---|---|---|---|
| `-h` | `--host` | server 地址 | `127.0.0.1` |
| `-p` | `--port` | server 端口 | `8765` |
| `-w` | `--warehouses` | 仓库数 W | `1` |
| `-S` | `--scale` | 行数缩放因子，**反向**：`1`=全量、`10`=1/10、`100`=1/100 | `1` |
| `-s` | `--skip-data` | 只建表+建索引，不灌数据（开发期调试用） | off |
| `-d` | `--drop` | 灌之前先 `DROP TABLE` 旧表（**不是** duration ⚠️） | off |

> ⚠️ **`-d` 在 loader 里是 drop，在 driver 里是 duration**——这是 RucBase 实验工具一个公认的小坑。脚本里千万**别把 driver 的 `-d 300` 复制粘贴给 loader**，否则会触发 drop 然后陷入意外状态。

#### 3.3.3 `tpcc_driver`

```
tpcc_driver [-h <host>] [-p <port>] [-w <W>] [-t <threads>] [-d <seconds>] [-S <scale>]
```

| 短选项 | 含义 | 默认 |
|---|---|---|
| `-h / -p` | server 地址 / 端口 | `127.0.0.1` / `8765` |
| `-w` | 仓库数（**必须**与 loader 一致） | `1` |
| `-t` | 并发客户端线程数 | `1` |
| `-d` | 测试时长（秒） | `30` |
| `-S` | 缩放因子（**必须**与 loader `-S` 一致） | `1` |

driver 不接受长选项，**全部是短选项**。

输出示例（scale=10 / -t 4 / -d 60，对应 `scripts/02_run_s10_t4_d60.sh`）：

```
=========== TPC-C Result ===========
duration:  61.6 s
committed: 340
aborted:   2720
TPS (commit/s):       5.52
tpmC (NewOrder/min):  149.04
latency p50/p95/p99 (us): 514 / 366624 / 4628129
```

### 3.4 数据规模选项

| `-S` | 数据量（实测） | 灌库时间 | 适合场景 |
|---|---|---|---|
| **100** | ~22 MB | ~1 秒 | **烟测**：开发期回归验证，几秒一轮 ✅ |
| **10** | ~236 MB | ~30 秒 | **统计基线**：5 分钟跑测能拿到 1k+ 样本，p50/p95 有意义 ⭐ |
| **1** | ~2.3 GB（含定长 slot 膨胀）| 2–3 分钟 | **标准 TPC-C W=1 全量**：暴露最终瓶颈，给学生优化的目标基线 |

### 3.5 三条铁律 ⚠️（不照做必丢数据）

1. **关 server 一律用 `kill -INT`，严禁 `kill -9`**。
   `kill -9` 跳过 `sm_manager_->close_db()`，BufferPool 中的脏页与刚更新的元数据全部不落盘，重启后看起来"整个库被清空了"——实际是数据页与目录元信息一起丢了，无法 recovery。

2. **做 golden 备份必须先 SIGINT 关 server**，再 `cp -a`。
   server 在跑时直接 `cp -a tpcc_db tpcc_db.golden_s1` 拷到的是不一致状态（脏页未落、`.meta` 还没写）；恢复后启动 recovery 会乱。**正确顺序：SIGINT → 等进程退出 → cp**。

3. **第二次跑测要回滚到干净状态时也必须先关 server**：
   ```bash
   kill -INT $(pgrep -f "rmdb tpcc_db")
   while pgrep -f "rmdb tpcc_db" >/dev/null; do sleep 0.5; done   # 等优雅退出
   rm -rf tpcc_db && cp -a tpcc_db.golden_s1 tpcc_db              # 秒级恢复
   ```
   省掉这一步直接覆盖正在打开的库目录，server 进程持有的 fd 还在写，结果是数据被踩花。

### 3.6 三脚本一键复现

仓库 `scripts/` 下提供了三个脚本，覆盖学生从拿到代码到出基线数字的全流程。**99% 的同学只需要按顺序跑这三条命令**：

```bash
cd /path/to/rucbase-lab
bash scripts/01_setup.sh                # 一次性准备（编译 + 灌库 + 制作 golden）
bash scripts/02_run_s10_t4_d60.sh       # 中规模 60 秒回归测（每次改完代码跑这条）
bash scripts/03_run_s1_t4_d300.sh       # 全量 5 分钟长压（出最终基线数字）
```

#### 脚本职责一览

| 脚本 | 做什么 | 预计耗时（首次） | 失败/缺依赖时的行为 |
|---|---|---|---|
| `01_setup.sh` | 检查 cmake/g++/端口 → 编译 → 灌 scale=10 → 灌 scale=1 → 各做一份 golden 备份 | **~3 分钟** | 任何一步失败都会打印日志路径 + 可读建议 |
| `02_run_s10_t4_d60.sh` | golden_s10 秒级还原 → 启 server → 跑 `-t 4 -d 60` → 摘要 | **~80 秒** | 缺二进制/缺 golden 时**自动调用 01_setup** |
| `03_run_s1_t4_d300.sh` | golden_s1 秒级还原 → 启 server → 后台跑 `-t 4 -d 300` + 心跳轮询 → 摘要 | **~6 分钟** | 同上，且超时会自动 SIGINT 兜底 |

#### 关键设计：幂等 + 自愈

- **重复跑不会出错**：`01_setup.sh` 默认跳过已存在的 golden；想强刷加 `--rebuild`。
- **学生忘了跑 01 也能救**：`02`/`03` 启动时检查二进制和 golden，缺什么就自动调用 `01_setup.sh --only=10` / `--only=1` 补上。
- **绝不 `kill -9`**：所有 server 退出都走 SIGINT，30 秒未退出会**报错让人工排查**而不是粗暴 kill（避免学生丢库）。
- **失败可读**：编译失败给 `/tmp/setup_build.log`、loader 失败给 `loader_s*.log`、server 起不来给 `server_*.log`，并在终端打印日志末尾 20~30 行。

#### 临时调参

两个跑测脚本都支持环境变量覆盖，无需改文件：

```bash
T=8 D=120 bash scripts/02_run_s10_t4_d60.sh    # 改成 8 线程 120 秒
T=2 D=600 bash scripts/03_run_s1_t4_d300.sh    # 改成 2 线程 10 分钟
```

#### 输出位置

所有产物都在 `build/dbroot/` 下：

| 文件 | 说明 |
|---|---|
| `tpcc_db/` | 当前工作库（每次跑测前会被 golden 覆盖） |
| `tpcc_db.golden_s1/`、`tpcc_db.golden_s10/` | golden 备份，**不要手动删** |
| `driver_s{S}_t{T}_d{D}.log` | driver 输出，结果在文件末尾 ~10 行 |
| `server_s{S}_t{T}_d{D}.log` | server 输出，排查崩溃看这里 |
| `loader_s{S}.log` | loader 输出（仅 setup 阶段产生） |


---

## 4. 基线性能数据

> **测试环境**：本机回环（127.0.0.1），server 与 driver 同机；基线代码即本仓库当前 HEAD，未做任何优化。锁机制为表级 S/X 锁 + wait-die 死锁预防。
>
> **数据来源**：以下两组数字直接来自 `scripts/02_run_s10_t4_d60.sh` 与 `scripts/03_run_s1_t4_d300.sh` 的输出；任何同学在同样硬件上跑这两个脚本都应得到接近的结果，可作为 sanity check。
>
> **统计实现说明**：driver 用蓄水池采样（每 100 笔记 1 笔）记录 latency，当样本数较小时 p95 与 p99 会落在同一桶，**这是 driver 的统计实现缺陷，可作为加分项之一改进**。

### 4.1 主基线（脚本 2 + 脚本 3）

| 配置 | 脚本 | duration | committed | aborted | abort 率 | TPS | **tpmC** | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| **scale=10 / -t 4 / -d 60** | `02_run_s10_t4_d60.sh` | 61.6 s | **340** | **2720** | 88.9% | 5.52 | **149.04** | 514 µs | 367 ms | 4628 ms |
| **scale=1  / -t 4 / -d 300** | `03_run_s1_t4_d300.sh` | 348.9 s | **79** | **455** | 85.2% | 0.23 | **6.11** | 2951 ms | 65870 ms | 65870 ms |

这两组是**写进文档的官方基线**，覆盖两个维度：

- **scale=10 / 4 线程 / 60 秒** → 中等规模快速回归测，每次改完代码用脚本 2 拿这条曲线；commit 数 340、abort 数 2720 一起体现了**当前 wait-die 表锁在中等并发下的真实代价**
- **scale=1 / 4 线程 / 300 秒** → 标准 TPC-C W=1 全量长压

### 4.2 重要观察

- ⚠️ **abort 率高（85–89%）是 wait-die 在 4 线程冲突下的正常表现**：年轻事务遇到老事务持锁会主动 abort，driver 重试后才能成功；这部分高 abort 正是"锁粒度过粗"的直接证据
- 🔬 **scale=1 长尾极端严重**：p95 = p99 ≈ 66 秒，意味着每隔几十秒就有事务被锁死或扫描死等到 driver 超时；这是后续优化时可以关注的尾延迟指标
- ✅ **稳定性 OK**：脚本 2 与脚本 3 在 4 线程下都能跑完，**没有死锁卡死**——wait-die 起到了应有作用，符合 Lab4 文档的预期行为

---
## 5. 参考优化方向

本章列出当前实现中主要的几个瓶颈点，以及可走的优化路径。**不要求全部完成**，挑 1～2 个方向做到可复现、有理论依据、有性能数据对比即可。

可以从 D1、D2 入手，实现代价低、提升明显。

### 5.1 主要瓶颈清单

| 编号 | 瓶颈 | 代码位置 | 表现 | 诊断依据 |
|---|---|---|---|---|
| **B1** | 全表扫描 | `src/optimizer/planner.cpp:33` `force_seq_scan = true` | 任何点查都走 SeqScan，stock/customer 表每查一条记录要扫几万页 | 在 `executor_seq_scan.h` 加 `printf` 或在 driver 输入 `EXPLAIN`，可看到所有 plan 都是 SeqScan |
| **B2** | 表级锁 | `src/transaction/concurrency/lock_manager.cpp` 只有 `lock_table()` 是真锁，`lock_*_on_record` 是 no-op | New-Order / Payment 与任意 SeqScan 互斥；wait-die 下年轻事务大量 die | 基线 abort 率 88.9% / 85.2% |
| **B3** | 锁升级冲突即 die | `lock_manager.cpp:101` "只要还有别人持锁就 abort" | 读后写场景（如 Payment 先 SELECT 后 UPDATE）会被领制 abort | 看 `aborted` 中 `DEADLOCK_PREVENTION` 原因占比 |
| **B4** | 无 WAL、abort 靠 `write_set` 物理回滚 | `transaction_manager.cpp:81` 的 `abort_txn` 倒序反做 INSERT/DELETE/UPDATE | 后台项：崩溃后不能恢复、commit 也不刷盘 | `commit_txn` 注释明说 "不做 group commit、不写 WAL" |


### 5.2 推荐优化路径（按性价比排序）

#### D1 启用索引扫描

#### D2 锁策略改进

### 5.3 延伸阅读：业界与学术界的优化思路

本节涉及一些前沿优化知识，供感兴趣的同学阅读。本节仅作参考，不做要求。

#### 5.3.1 锁与并发控制协议本身

近 10 年的趋势是**从纯 2PL 走向 MVCC + OCC + 自适应混合**，本实验的"表锁 + wait-die"对应的是 1970 年代 System R 的设计点。可以把下面这张表当作一份索引：

| 协议方向 | 代表工作 | 核心思想 | 与本实验的关系 |
|---|---|---|---|
| **MVCC + Serializable** | Hekaton (SIGMOD'13)、HyPer/Umbra (TUM)、Cicada (SIGMOD'17) | 写不阻塞读，靠 timestamp + validation 决定可见性 | 直接消解 D2 中"读写互斥"导致的 abort 风暴 |
| **OCC + 高效 validation** | Silo (SOSP'13)、TicToc (SIGMOD'16)、MOCC (VLDB'16) | 无中心化锁表，commit 时再检查冲突；TicToc 用动态时间戳避免假冲突 | 提供"abort 率高 ≠ 一定要悲观锁"的另一条思路 |
| **混合 / 自适应协议** | IC3 (SIGMOD'16)、Bamboo (SIGMOD'21)、CormCC (SIGMOD'18) | 不同事务用不同协议；Bamboo 在 2PL 上做 early lock release | 让冲突链上的等待者尽早往前推，对 TPC-C 热点行场景非常对症 |
| **确定性执行** | Calvin (SIGMOD'12)、Aria (VLDB'20)、Caracal (SOSP'21) | 提前定序消除死锁，副作用是丢了交互式事务 | 思路上的"另一极"：根本不让冲突发生 |
| **Range / phantom 处理** | MaaT、ERMIA、Umbra Precision Locks (VLDB'20) | 干净处理 SI/SSI 下的 phantom，而不是用表锁糊过去 | 解释\"为什么纯行锁还不够\"，引出谓词锁/范围锁 |

**死锁预防的演进**：经典的 wait-die / wound-wait（即本实验所用）已知**饥饿、年轻事务被反复重启**。新一些的工作如 **Bamboo (SIGMOD'21)** 通过 early lock release 减少链式等待；**Polyjuice (OSDI'21)** 干脆用 RL 学每次访问的"等/退/跳"策略，在 TPC-C 上同时压过 Silo / Cicada / 2PL。


#### 5.3.2 事务路径上的工程优化

工业数据库这几年真正落到生产的优化，几乎都不在"协议"层，而在**提交路径、索引并发、缓冲池、分布式架构**这些工程位置。

##### (a) 提交路径

| 优化 | 代表落地 | 对本实验的启发 |
|---|---|---|
| **Group commit / pipeline log** | MySQL InnoDB、PostgreSQL、TiDB、OceanBase | 已是商用 DB 标配；本实验目前**根本没写 WAL**，是 D 系列后续的天然扩展 |
| **远端 redo（log-as-a-service）** | Aurora (SIGMOD'17/'18)、Socrates (SIGMOD'19)、PolarDB Serverless (SIGMOD'21) | "log = the database"，把 redo 下推到存储 |
| **Persistent Memory WAL** | SOFORT (HyPer)、Pronto | Optane EOL 后热度下降，但 CXL 内存把这个题目又带回来了 |
| **NIC offload / SmartNIC commit** | XStore (OSDI'20)、LineFS (SOSP'21) | 用 DPU 把 commit fsync 路径搬出 CPU |

##### (b) 索引与缓冲池

- **Learned Index**（Kraska et al., SIGMOD'18）→ ALEX、PGM、LIPP、Updatable LIPP（VLDB'22~24）
- **B+树 → Bw-tree (Hekaton) / OLC-Btree** —— lock-free 或 optimistic latch coupling
- **LeanStore (ICDE'18) / Umbra (CIDR'20)** —— 摆脱 buffer pool latch 瓶颈，page eviction 用 epoch + pointer swizzling
- **缓存替换学习化**：LRB (OSDI'20)、CACHEUS、HALP (NSDI'23)

##### (c) 分布式事务

- **Spanner / TrueTime**（OSDI'12）启发了 CockroachDB、YugabyteDB
- **FaRM / FaRMv2**（SOSP'15、SOSP'19）—— RDMA + 乐观分布式事务
- **DrTM 系列**（SOSP'15、ATC'18、DrTM+H OSDI'22）—— RDMA + HTM
- 国内云厂商（OceanBase / TiDB / PolarDB-X）在 TPC-C 上反复刷榜，主线就是 **MVCC + Paxos/Raft + 分区 2PC**


#### 5.3.3 Learned Concurrency Control



| 工作 | 出处 | 核心思想 |
|---|---|---|
| **Polyjuice** | OSDI'21（MIT） | 把每笔事务的每次访问看作一次"等 / 不等 / abort"决策，用进化策略 + 离线训练学最优策略；TPC-C 上明显优于 Silo / Cicada / 2PL |
| **CormCC / Tebaldi** | SIGMOD'18 / VLDB'19 | 协议自动选择，可看作浅层版 meta-controller：热点表用 OCC、冷表用 2PL |
| **Bamboo** | SIGMOD'21 | 不算严格意义的 learned，但属于"自适应放松 2PL"这一脉 |

**这条线开放的问题**（也是本实验的天然延伸）：

1. **基于归因的事务回滚**：现在 abort 是粗粒度回滚整个事务，能否只回滚冲突子集（partial rollback）？基础工作是 ARIES nested savepoint，但学习型方案还没成形
2. **RL 学习的事务调度**：Polyjuice 之后还没有真正的后续 SOTA，**离线 RL + LLM 提供的领域先验**是一个干净的切口
3. **LLM 驱动的死锁/锁等待诊断**：把 `pg_locks` / `INNODB_LOCK_WAITS` 的时间序列喂给 Agent，让它给出"长事务/索引缺失/隔离级别过高"等可解释定位——工业界（Aurora Performance Insights、PolarDB AutoPilot）已上线，但归因深度仍然不够

> 如果你的 D2 优化最终选择"用某种启发式动态决定加锁/放锁时机"，就可以把它包装成"轻量级 learned CC 雏形"——这比单纯说"我把 wait-die 改成了 wait-die+"在报告里好讲得多。

---

## 6. 常见问题

> **优先级建议**：跑测出现异常时先看 Q1~Q5（运行环境），出现性能/正确性疑问看 Q6~Q9，做优化遇到 abort/死锁看 Q10~Q12。

---

### Q1: loader 报 `connection refused` / driver 报 `gethostbyname failed`？

按这个顺序排查：

```bash
# 1. 看 server 是否还在跑
pgrep -af "rmdb tpcc_db"

# 2. 看 8765 端口是否真的在监听
ss -ltn | grep 8765

# 3. 看 server 的启动日志（结果一般在文件末尾）
tail -30 build/dbroot/server.log              # 01_setup 阶段
tail -30 build/dbroot/server_s10_t4_d60.log   # 02 跑测时
```

最常见的两个根因：

| 现象 | 解决 |
|---|---|
| `pgrep` 没结果 | server 启动失败，看 `server.log` 末尾的报错（多半是上次没干净退出，library 文件锁残留 → 删 `dbroot/tpcc_db` 重灌即可） |
| `pgrep` 有结果但端口不通 | server 正在 `close_db` 落盘（或灌库阶段在 fsync），等几秒重试 |

**不要直接 `kill -9`** ——会丢库（详见 Q11）。

### Q2: scale=1 灌库一直卡在 stock 表？

属于正常现象。stock 表 10 万行 × 310 字节，按当前定长 slot 实现要写 ~95k 个 4 KB 页 + 索引维护，单机预计 **2~3 分钟**（SSD）/ **5~8 分钟**（机械盘）。

判断"是真的慢"还是"卡死"：

```bash
# 看 server 进程 CPU 占用：>50% 就说明它还在干活
top -p $(pgrep -f "rmdb tpcc_db") -b -n 1 | tail -3

# 看 dbroot 大小是否还在涨
watch -n 5 'du -sh build/dbroot/tpcc_db'
```

如果超过 15 分钟还没完成，检查：(1) `dbroot` 所在磁盘是否真的有 ≥ 5 GB 空闲；(2) 内存是否被换页（`free -h` 看 `swap used`）。

### Q3: 跑测时 abort 率高达 80%+，是不是有 bug？

**不是 bug，是当前实现的预期行为**。第 1.3.1 节已说明：本实验用 wait-die 死锁预防，年轻事务一旦撞上老事务持锁就 abort。在 4 线程并发下：

| 配置 | 实测 abort 率 | 解读 |
|---|---|---|
| `scale=10 / -t 4 / -d 60` | 88.9% | 表锁 + 4 线程，年轻事务几乎必 die |
| `scale=1 / -t 4 / -d 300` | 85.2% | 同上，扫描时间长但抢锁逻辑相同 |

**真正的 bug 信号**反而是：

- `committed = 0`：通常是事务未正确提交（看 `executor_*.h` 是否漏掉 `txn_->set_state(COMMITTED)`）
- `aborted` 中夹杂 `Connection reset` 之类 server 端崩溃 → 看 `server_*.log`


### Q4: 优化后 abort 率反而升高了？

最常见的几种原因（按命中频率排）：

1. **改成了 no-wait**：把 wait-die 简化成"撞锁就 abort"，吞吐立刻塌方（参考 1.3.2 表格里的对比）
2. **锁粒度切到行级但漏了表级 IS/IX**：UPDATE 在某行加 X，扫描者却用全表 SeqScan 触发表级 S 锁互斥，全表事务被批量 die
3. **commit 路径漏放锁**：所有锁必须在 `transaction_manager::commit()` 时一次性 release，并通知等待队列；漏写就把所有后续事务都饿死成 abort
4. **死锁检测线程触发回滚**：如果你额外实现了检测，先确认它**不是把不该回滚的事务也判成了死锁参与者**

### Q5: 我能不能直接改 `rm_file_handle.cpp` 把 record_size 改小？

不行。`record_size` 是从 schema 计算出来的，必须和 `select/insert` 路径中按 offset 解析字段保持一致——一处改小了别处就读出脏数据。要改"变长记录"必须同步重构 slot 结构、page header、bitmap 与所有 reader/writer，这是一个完整改造。

如果只是想让 scale=1 灌库快一点，正确思路是：(1) 先让 IndexScan 跑起来减少扫描时间，(2) 用 scale=10 做日常调试，仅在最终交付测一次 scale=1。

### Q6: 我做完 Lab2 之后，是否一定能跑出 IndexScan 收益？

不一定。还需要确认：

1. **B+ 树正确支持复合 key**（TPC-C 索引几乎都是 2~3 列复合，如 `stock(s_w_id, s_i_id)`）
2. **`src/execution/executor_index_scan.h` 中 `key` 拼接逻辑正确**：把多列按 schema offset 拼成连续字节串
3. **打开 planner 开关**：`src/optimizer/planner.cpp` 里的 `force_seq_scan = true` 改成 `false`
4. **`get_index_cols` 能匹配到 WHERE 列序**：复合索引 `(a, b)` 必须 WHERE 同时含 `a` 和 `b` 才能走索引

建议先跑 `src/test/index/` 下的 B+ 树单测验证索引本身正确，再到 TPC-C 验证端到端收益。

### Q7: 端口 8765 被占用怎么办？

```bash
# 1. 查谁占着
ss -ltnp 2>/dev/null | grep 8765
# 或
lsof -i :8765

# 2. 如果是上次跑测残留的 rmdb，用 SIGINT 优雅关
pkill -INT -f "rmdb tpcc_db"

# 3. 等它真退出（不要立即重启 server）
while pgrep -f "rmdb tpcc_db" >/dev/null; do sleep 0.5; done
```

如果端口被**别的程序**占了（比如同机器上跑了别的 RucBase 实例），改 `src/rmdb.cpp` 里的端口宏 + driver 的 `-p` 参数同步修改即可。

### Q8: 跑测脚本卡住、终端长时间没输出怎么办？

**先判断是 hang 还是慢**：

```bash
# 看心跳输出（02/03 脚本每 25s 报活一次）
tail -f build/dbroot/run_*.log

# 看 server 的 CPU 占用
top -p $(pgrep -f "rmdb tpcc_db")
```

| 现象 | 含义 | 处理 |
|---|---|---|
| CPU > 50%、心跳还在 | 真的在跑，等就行 | 不动 |
| CPU ~0%、心跳还在 | driver 等 server 响应，server 内部死锁了 | 看 5.1 B1，记录现象给报告 |
| 心跳停止超过 1 分钟 | 脚本/shell 异常退出 | `pkill -INT -f tpcc_driver`，重跑 |

**`scripts/03_run_s1_t4_d300.sh` 内置了 `D + 180` 秒超时兜底**，正常情况下不会真死。如果你改了脚本拿掉了 timeout，记得手动加回。

### Q9: 为什么 driver 输出里 p95 == p99？

driver 用**蓄水池采样**：每 100 笔事务记录 1 笔的 latency。当总 commit 数较少时（如 scale=1 跑出 79 笔），采样池只有 ~1 个尾部样本，p95 和 p99 自然落到同一桶。


### Q10: 跑了 `02_run_s10_t4_d60.sh` 直接报"golden_s10 missing"，但我跑过 setup 啊？

按这个顺序检查：

```bash
# 1. golden 在不在
ls -la build/dbroot/ | grep golden

# 2. setup 是不是真跑成功了？看末尾摘要
tail -20 build/dbroot/loader_s10.log
```

最常见的两个坑：

- **setup 在灌库中途被 Ctrl+C**：golden 没建成。重跑 `bash scripts/01_setup.sh --only=10`
- **手动 `rm` 删过 dbroot**：把 golden 一起删了。重跑 setup 即可

`02`/`03` 脚本会**自动调用** `01_setup.sh --only=N` 补齐 golden（见 3.6 节"幂等 + 自愈"），所以即便首次跑 02/03 也能用，只是会多等 30 秒到 3 分钟。

### Q11: tpcc_db 突然变得很小 / 报 schema 错误，库被搞坏了？

99% 是被 `kill -9` 导致——SIGKILL 跳过 `sm_manager_->close_db()`，BufferPool 中的脏页和 `.meta` 元信息一起没落盘，重启后看起来"整个库被清空了"。

恢复方法：

```bash
# 用 golden 直接覆盖
pkill -INT -f "rmdb tpcc_db" 2>/dev/null
while pgrep -f "rmdb tpcc_db" >/dev/null; do sleep 0.5; done
rm -rf build/dbroot/tpcc_db
cp -a build/dbroot/tpcc_db.golden_s1 build/dbroot/tpcc_db   # 或 golden_s10
```

如果连 golden 都丢了：`bash scripts/01_setup.sh --rebuild`，从头重做（约 3 分钟）。

**预防方法**：终生只用 `kill -INT`，绝不 `kill -9`（见 3.5 节"三条铁律"）。

### Q12: 文档说 X，但代码看着是 Y，听谁的？

**听代码的，不听文档的**——本实验是迭代加内容，文档可能滞后于最新一次重构。具体到几个高频不一致点：

| 文档 | 代码事实（截至本仓库当前 HEAD） |
|---|---|
| 5.1 节"全局大锁" | 已改成表级 S/X 锁 + wait-die（见 1.3） |
| 老版本提到 `force_seq_scan` 默认 false | 当前**强制为 true**（`planner.cpp` 第 33 行） |
| 5.2 节伪代码 `lock_manager_->global_lock()` | 这个接口已是 no-op，真正的锁逻辑在 `lock_manager.cpp::lock_table` |

如果你发现新的不一致，优先信 `git log --oneline src/transaction/ src/execution/` 最近 5 次提交说明的事实，并在报告里标注"以代码为准"。

---

## 附录 A：关键源文件速查

```
事务系统
├── src/transaction/transaction_manager.{h,cpp}   begin/commit/abort
├── src/transaction/transaction.h                 Transaction / write_set / lock_set
├── src/transaction/txn_defs.h                    WriteRecord 类型
└── src/transaction/concurrency/lock_manager.*   全局锁 / 表锁 / 行锁

执行器
├── src/execution/execution_manager.cpp           SQL 入口路由
├── src/execution/executor_{insert,update,delete}.h  写 write_set
└── src/execution/executor_{seq,index}_scan.h     扫描算子

优化器
└── src/optimizer/planner.cpp                     force_seq_scan 开关

存储
├── src/record/rm_file_handle.*                   定长 slot（B3 瓶颈所在）
└── src/index/ix_*                                B+ 树（Lab2，当前未实现）

TPC-C
└── src/test/tpcc/                                独立 benchmark 工具链
```

---

**祝同学们玩得开心，做出漂亮的优化曲线！**

