/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    /**
     * @brief 构建表迭代器scan_,并开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void beginTuple() override {
        // 表级 S 锁：纯读路径加共享锁，让多个只读事务可以并发，
        // 同时与并发的 UPDATE/DELETE/INSERT（X 锁）保持互斥；
        // 若同一事务后续走到 update/delete/insert，会触发 S→X 升级，
        // lock_manager 已对升级冲突走死锁预防（abort）。
        if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }
        scan_ = std::make_unique<RmScan>(fh_);
        // 找到第一个满足谓词条件的元组
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                break;
            }
            scan_->next();
        }
    }

    /**
     * @brief 从当前scan_指向的记录开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void nextTuple() override {
        // 从当前scan_指向的记录开始迭代扫描
        for (scan_->next(); !scan_->is_end(); scan_->next()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                break;
            }
        }
    }

    /**
     * @brief 返回下一个满足扫描条件的记录
     *
     * @return std::unique_ptr<RmRecord>
     */
    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override { return scan_->is_end(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    // 判断单个条件是否满足
    bool eval_cond(const std::vector<ColMeta> &rec_cols, const Condition &cond, const RmRecord *rec) {
        auto lhs_col = get_col(rec_cols, cond.lhs_col);
        char *lhs_data = rec->data + lhs_col->offset;
        ColType lhs_type = lhs_col->type;
        int lhs_len = lhs_col->len;

        char *rhs_data;
        ColType rhs_type;
        int rhs_len;
        // 如果右边是值
        if (cond.is_rhs_val) {
            rhs_type = cond.rhs_val.type;
            rhs_len = lhs_len;  // 用左边的长度
            // 需要获取原始数据
            rhs_data = cond.rhs_val.raw->data;
        } else {
            // 右边是列
            auto rhs_col = get_col(rec_cols, cond.rhs_col);
            rhs_data = rec->data + rhs_col->offset;
            rhs_type = rhs_col->type;
            rhs_len = rhs_col->len;
        }

        // 比较
        int cmp = 0;
        if (lhs_type == TYPE_INT && rhs_type == TYPE_INT) {
            int lhs_val = *(int *)lhs_data;
            int rhs_val = *(int *)rhs_data;
            cmp = (lhs_val > rhs_val) - (lhs_val < rhs_val);
        } else if (lhs_type == TYPE_FLOAT && rhs_type == TYPE_FLOAT) {
            float lhs_val = *(float *)lhs_data;
            float rhs_val = *(float *)rhs_data;
            cmp = (lhs_val > rhs_val) - (lhs_val < rhs_val);
        } else if (lhs_type == TYPE_STRING && rhs_type == TYPE_STRING) {
            int min_len = std::min(lhs_len, rhs_len);
            cmp = memcmp(lhs_data, rhs_data, min_len);
        } else if (lhs_type == TYPE_INT && rhs_type == TYPE_FLOAT) {
            float lhs_val = (float)(*(int *)lhs_data);
            float rhs_val = *(float *)rhs_data;
            cmp = (lhs_val > rhs_val) - (lhs_val < rhs_val);
        } else if (lhs_type == TYPE_FLOAT && rhs_type == TYPE_INT) {
            float lhs_val = *(float *)lhs_data;
            float rhs_val = (float)(*(int *)rhs_data);
            cmp = (lhs_val > rhs_val) - (lhs_val < rhs_val);
        } else {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }

        switch (cond.op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default: throw InternalError("Unexpected op type");
        }
    }

    // 判断所有条件是否满足（AND）
    bool eval_conds(const std::vector<ColMeta> &rec_cols, const std::vector<Condition> &conds, const RmRecord *rec) {
        for (auto &cond : conds) {
            if (!eval_cond(rec_cols, cond, rec)) {
                return false;
            }
        }
        return true;
    }

    Rid &rid() override { return rid_; }
};