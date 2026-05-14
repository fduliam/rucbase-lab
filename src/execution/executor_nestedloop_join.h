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

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    void beginTuple() override {
        left_->beginTuple();
        if (!left_->is_end()) {
            right_->beginTuple();
        }
        // 找到第一个满足条件的组合
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                // 检查是否满足join条件
                if (eval_join_conds()) {
                    return;  // 找到第一个满足条件的元组
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (!left_->is_end()) {
                right_->beginTuple();
            }
        }
        isend = true;
    }

    void nextTuple() override {
        // 先推进右表
        right_->nextTuple();
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                if (eval_join_conds()) {
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (!left_->is_end()) {
                right_->beginTuple();
            }
        }
        isend = true;
    }

    std::unique_ptr<RmRecord> Next() override {
        auto left_rec = left_->Next();
        auto right_rec = right_->Next();
        // 拼接左右记录
        auto joined_rec = std::make_unique<RmRecord>(len_);
        memcpy(joined_rec->data, left_rec->data, left_->tupleLen());
        memcpy(joined_rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return joined_rec;
    }

    bool is_end() const override { return isend; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

private:
    // 判断单个条件
    int compare(char *lhs_data, char *rhs_data, ColType type, int len) {
        if (type == TYPE_INT) {
            int lv = *(int *)lhs_data, rv = *(int *)rhs_data;
            return (lv > rv) - (lv < rv);
        } else if (type == TYPE_FLOAT) {
            float lv = *(float *)lhs_data, rv = *(float *)rhs_data;
            return (lv > rv) - (lv < rv);
        } else {
            return memcmp(lhs_data, rhs_data, len);
        }
    }

    bool eval_join_conds() {
        auto left_rec = left_->Next();
        auto right_rec = right_->Next();
        for (auto &cond : fed_conds_) {
            // 获取左边数据
            auto lhs_col = get_col(cols_, cond.lhs_col);
            char *lhs_data;
            if ((size_t)lhs_col->offset < left_->tupleLen()) {
                lhs_data = left_rec->data + lhs_col->offset;
            } else {
                lhs_data = right_rec->data + lhs_col->offset - left_->tupleLen();
            }

            char *rhs_data;
            ColType rhs_type;
            int cmp_len = lhs_col->len;
            if (cond.is_rhs_val) {
                rhs_data = cond.rhs_val.raw->data;
                rhs_type = cond.rhs_val.type;
            } else {
                auto rhs_col = get_col(cols_, cond.rhs_col);
                if ((size_t)rhs_col->offset < left_->tupleLen()) {
                    rhs_data = left_rec->data + rhs_col->offset;
                } else {
                    rhs_data = right_rec->data + rhs_col->offset - left_->tupleLen();
                }
                rhs_type = rhs_col->type;
                cmp_len = std::min(lhs_col->len, rhs_col->len);
            }

            int cmp = compare(lhs_data, rhs_data, lhs_col->type, cmp_len);
            bool result = false;
            switch (cond.op) {
                case OP_EQ: result = (cmp == 0); break;
                case OP_NE: result = (cmp != 0); break;
                case OP_LT: result = (cmp < 0); break;
                case OP_GT: result = (cmp > 0); break;
                case OP_LE: result = (cmp <= 0); break;
                case OP_GE: result = (cmp >= 0); break;
            }
            if (!result) return false;
        }
        return true;
    }

    Rid &rid() override { return _abstract_rid; }
};