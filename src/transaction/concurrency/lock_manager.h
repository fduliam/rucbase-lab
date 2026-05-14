/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. ... */

#pragma once

#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <list>
#include "transaction/transaction.h"

static const std::string GroupLockModeStr[10] = {"NON_LOCK", "IS", "IX", "S", "X", "SIX"};

/**
 * @brief 表级 S/X 锁 + wait-die 死锁预防（最小可行版本）
 *
 * 设计取舍：
 *   - 只做表锁，不做行锁：对 TPC-C 来说粒度仍然偏粗，但已经能让"读读并发、不同表读写并发"
 *   - 只支持 S（共享）/ X（独占）两种模式，不实现 IS/IX/SIX
 *   - 锁升级：持 S 的事务再请求 X 时，等其它持锁者释放后升级
 *   - 死锁预防：wait-die（老事务可以等，年轻事务直接 abort）
 *   - 严格 2PL：commit/abort 时由 TransactionManager 统一释放
 *
 * 保留 global_lock/global_unlock 仅为兼容旧调用（已改为 no-op），
 * 以及保留行级/IS/IX 接口为 no-op 以避免改动其它调用方。
 */
class LockManager {
public:
    enum class LockMode { SHARED, EXLUCSIVE, INTENTION_SHARED, INTENTION_EXCLUSIVE, S_IX };
    enum class GroupLockMode { NON_LOCK, IS, IX, S, X, SIX };

    class LockRequest {
    public:
        LockRequest(txn_id_t txn_id, timestamp_t ts, LockMode lock_mode)
            : txn_id_(txn_id), start_ts_(ts), lock_mode_(lock_mode), granted_(false) {}
        txn_id_t txn_id_;
        timestamp_t start_ts_;       // 用于 wait-die
        LockMode lock_mode_;
        bool granted_;
    };

    class LockRequestQueue {
    public:
        std::list<LockRequest> request_queue_;
        std::condition_variable cv_;
        // 当前已授予的锁的"组模式"：NON_LOCK / S / X
        GroupLockMode group_lock_mode_ = GroupLockMode::NON_LOCK;
    };

public:
    LockManager() {}
    ~LockManager() {}

    // ===== 表级 S/X 锁（真正生效）=====
    bool lock_shared_on_table(Transaction* txn, int tab_fd);
    bool lock_exclusive_on_table(Transaction* txn, int tab_fd);

    // ===== 行级锁、IS、IX：保留 no-op，避免影响其它调用方 =====
    bool lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd);
    bool lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd);
    bool lock_IS_on_table(Transaction* txn, int tab_fd);
    bool lock_IX_on_table(Transaction* txn, int tab_fd);

    // ===== 释放：被 TransactionManager 在 commit/abort 末尾调用 =====
    bool unlock(Transaction* txn, LockDataId lock_data_id);

    // ===== 兼容旧接口：现已 no-op，事务并发由 lock_table_ 管 =====
    void global_lock()   { /* no-op */ }
    void global_unlock() { /* no-op */ }

private:
    // 内部：表级加锁通用逻辑（mode = S 或 X）
    bool lock_table(Transaction* txn, int tab_fd, LockMode mode);

    std::mutex latch_;                                          // 保护 lock_table_ 自身
    std::unordered_map<LockDataId, LockRequestQueue> lock_table_;
};
