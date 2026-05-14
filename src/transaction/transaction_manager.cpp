/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"
#include "index/ix.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 *
 * 备注：
 *   1. 没有 WAL（不写 BEGIN log）
 *   2. 并发由 LockManager 的表级 S/X 锁 + wait-die 死锁预防保证
 *   3. 严格 2PL：所有锁在 commit/abort 时统一释放
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    std::unique_lock<std::mutex> lock(latch_);
    if (txn == nullptr) {
        txn_id_t new_id = next_txn_id_.fetch_add(1);
        txn = new Transaction(new_id);
        txn->set_start_ts(next_timestamp_.fetch_add(1));
    }
    txn->set_state(TransactionState::GROWING);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 *
 * 实现说明：直接释放锁、清空 write_set，不做 group commit、不写 WAL。
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) return;

    // 1. 清空写集（提交不需要回滚信息）
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        delete write_set->back();
        write_set->pop_back();
    }

    // 2. 释放该事务持有的所有锁
    auto lock_set = txn->get_lock_set();
    for (const auto &lock_id : *lock_set) {
        lock_manager_->unlock(txn, lock_id);
    }
    lock_set->clear();

    // 3. 更新状态
    txn->set_state(TransactionState::COMMITTED);

    // 4. 从全局事务表中移除
    {
        std::unique_lock<std::mutex> lock(latch_);
        txn_map.erase(txn->get_transaction_id());
    }
    delete txn;
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 *
 * 实现说明：根据 write_set 倒序物理回滚（INSERT->delete, DELETE->insert, UPDATE->old）。
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) return;

    // 1. 倒序回滚写操作
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        WriteRecord *wr = write_set->back();
        write_set->pop_back();
        const std::string &tab_name = wr->GetTableName();
        const Rid &rid = wr->GetRid();

        // 找不到表/索引就跳过（防御性）
        if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) {
            delete wr;
            continue;
        }
        RmFileHandle *fh = sm_manager_->fhs_.at(tab_name).get();
        TabMeta &tab = sm_manager_->db_.get_table(tab_name);

        switch (wr->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                // 回滚 insert：删除该 rid 对应的索引项与记录
                auto rec = fh->get_record(rid, nullptr);
                for (auto &index : tab.indexes) {
                    auto ih = sm_manager_->ihs_.at(
                        sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                    char *key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t j = 0; j < (size_t)index.col_num; ++j) {
                        memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                        offset += index.cols[j].len;
                    }
                    ih->delete_entry(key, txn);
                    delete[] key;
                }
                fh->delete_record(rid, nullptr);
                break;
            }
            case WType::DELETE_TUPLE: {
                // 回滚 delete：把记录与索引项都补回去
                fh->insert_record(rid, wr->GetRecord().data);
                for (auto &index : tab.indexes) {
                    auto ih = sm_manager_->ihs_.at(
                        sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                    char *key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t j = 0; j < (size_t)index.col_num; ++j) {
                        memcpy(key + offset,
                               wr->GetRecord().data + index.cols[j].offset,
                               index.cols[j].len);
                        offset += index.cols[j].len;
                    }
                    ih->insert_entry(key, rid, txn);
                    delete[] key;
                }
                break;
            }
            case WType::UPDATE_TUPLE: {
                // 回滚 update：先把当前记录的索引删除，再写回旧记录与旧索引
                auto cur_rec = fh->get_record(rid, nullptr);
                for (auto &index : tab.indexes) {
                    auto ih = sm_manager_->ihs_.at(
                        sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                    char *cur_key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t j = 0; j < (size_t)index.col_num; ++j) {
                        memcpy(cur_key + offset, cur_rec->data + index.cols[j].offset, index.cols[j].len);
                        offset += index.cols[j].len;
                    }
                    ih->delete_entry(cur_key, txn);
                    delete[] cur_key;
                }
                fh->update_record(rid, wr->GetRecord().data, nullptr);
                for (auto &index : tab.indexes) {
                    auto ih = sm_manager_->ihs_.at(
                        sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                    char *old_key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t j = 0; j < (size_t)index.col_num; ++j) {
                        memcpy(old_key + offset,
                               wr->GetRecord().data + index.cols[j].offset,
                               index.cols[j].len);
                        offset += index.cols[j].len;
                    }
                    ih->insert_entry(old_key, rid, txn);
                    delete[] old_key;
                }
                break;
            }
        }
        delete wr;
    }

    // 2. 释放锁
    auto lock_set = txn->get_lock_set();
    for (const auto &lock_id : *lock_set) {
        lock_manager_->unlock(txn, lock_id);
    }
    lock_set->clear();

    // 3. 更新状态
    txn->set_state(TransactionState::ABORTED);

    // 4. 从全局事务表中移除
    {
        std::unique_lock<std::mutex> lock(latch_);
        txn_map.erase(txn->get_transaction_id());
    }
    delete txn;
}