/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"
#include "transaction/txn_defs.h"

/**
 * 表级 S/X 锁 + wait-die：
 *   - granted 为空：直接授予并设置 group_lock_mode_
 *   - 兼容（都是 S 且新请求也是 S）：直接授予
 *   - 不兼容：按 wait-die 决定等还是 abort
 *
 * 锁升级（同一事务持 S 再请求 X）：
 *   - 若队列里此事务唯一持锁者：直接升级
 *   - 否则同样按 wait-die 等其他持锁者
 */

// 行级 / IS / IX：no-op，但仍把 id 塞进 lock_set，便于 TransactionManager 释放
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) return true;
    LockDataId id(tab_fd, rid, LockDataType::RECORD);
    txn->get_lock_set()->insert(id);
    return true;
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) return true;
    LockDataId id(tab_fd, rid, LockDataType::RECORD);
    txn->get_lock_set()->insert(id);
    return true;
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    // 把 IS 当 S 处理（最小实现：意向锁不区分）
    return lock_shared_on_table(txn, tab_fd);
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    // 把 IX 当 X 处理
    return lock_exclusive_on_table(txn, tab_fd);
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock_table(txn, tab_fd, LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock_table(txn, tab_fd, LockMode::EXLUCSIVE);
}

bool LockManager::lock_table(Transaction* txn, int tab_fd, LockMode mode) {
    if (txn == nullptr) return true;
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(),
                                        AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId id(tab_fd, LockDataType::TABLE);
    std::unique_lock<std::mutex> lk(latch_);
    auto &q = lock_table_[id];

    // 1) 是否已经持有该锁（幂等 / 升级）
    LockRequest *self = nullptr;
    for (auto &req : q.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
            self = &req;
            break;
        }
    }
    if (self != nullptr) {
        // 已持 X：随便都满足
        if (self->lock_mode_ == LockMode::EXLUCSIVE) return true;
        // 已持 S 且申请 S：满足
        if (self->lock_mode_ == LockMode::SHARED && mode == LockMode::SHARED) return true;
        // 已持 S 且申请 X：升级
        // 简化策略：如果还有别人持锁，直接 die（避免两个事务同时升级造成的微姙死锁）
        bool only_me = true;
        for (auto &req : q.request_queue_) {
            if (req.granted_ && req.txn_id_ != txn->get_transaction_id()) {
                only_me = false;
                break;
            }
        }
        if (only_me) {
            self->lock_mode_ = LockMode::EXLUCSIVE;
            q.group_lock_mode_ = GroupLockMode::X;
            return true;
        }
        // 不论老者年轻者，只要有冲突就升级失败 abort，交由应用重试
        throw TransactionAbortException(txn->get_transaction_id(),
                                        AbortReason::DEADLOCK_PREVENTION);
    }

    // 2) 全新请求
    auto compatible = [&](GroupLockMode gm, LockMode want) {
        if (gm == GroupLockMode::NON_LOCK) return true;
        if (gm == GroupLockMode::S && want == LockMode::SHARED) return true;
        return false;
    };

    // wait-die：若不兼容，比较时间戳
    while (!compatible(q.group_lock_mode_, mode)) {
        // 检查所有已持锁事务，自己是否最老
        for (auto &req : q.request_queue_) {
            if (req.granted_ && req.txn_id_ != txn->get_transaction_id()) {
                if (txn->get_start_ts() >= req.start_ts_) {
                    // 自己更年轻 → die
                    throw TransactionAbortException(txn->get_transaction_id(),
                                                    AbortReason::DEADLOCK_PREVENTION);
                }
            }
        }
        // 自己更老：等
        q.cv_.wait(lk);
    }

    // 授予
    q.request_queue_.emplace_back(txn->get_transaction_id(), txn->get_start_ts(), mode);
    q.request_queue_.back().granted_ = true;
    if (mode == LockMode::EXLUCSIVE) {
        q.group_lock_mode_ = GroupLockMode::X;
    } else if (q.group_lock_mode_ == GroupLockMode::NON_LOCK) {
        q.group_lock_mode_ = GroupLockMode::S;
    }
    txn->get_lock_set()->insert(id);
    return true;
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) return true;
    // 行级 / IS / IX 锁是 no-op，无需操作真锁表
    if (lock_data_id.type_ == LockDataType::RECORD) return true;

    std::unique_lock<std::mutex> lk(latch_);
    auto it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) return true;
    auto &q = it->second;

    // 删除该事务的所有请求
    bool has_x = false, has_s = false;
    for (auto rit = q.request_queue_.begin(); rit != q.request_queue_.end();) {
        if (rit->txn_id_ == txn->get_transaction_id()) {
            rit = q.request_queue_.erase(rit);
        } else {
            if (rit->granted_) {
                if (rit->lock_mode_ == LockMode::EXLUCSIVE) has_x = true;
                else if (rit->lock_mode_ == LockMode::SHARED) has_s = true;
            }
            ++rit;
        }
    }
    // 重算 group_lock_mode_
    if (has_x)        q.group_lock_mode_ = GroupLockMode::X;
    else if (has_s)   q.group_lock_mode_ = GroupLockMode::S;
    else              q.group_lock_mode_ = GroupLockMode::NON_LOCK;

    // 唤醒等待者
    q.cv_.notify_all();
    return true;
}