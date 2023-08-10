/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <algorithm>
#include <list>
#include <vector>

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "wrappers/item_wrapper.h"
#include "wrappers/mock_txn_op.h"

#include <iostream>

namespace {

/*
 * __txn_mod_sortable_key --
 *     Given an operation return a boolean indicating if it has a sortable key.
 */
static inline bool
__txn_mod_key_op(MockTxnOp *opt)
{
    std::cout << "opt type" << opt->op_type << std::endl;
    switch (opt->op_type) {
    case (WT_TXN_OP_NONE):
    case (WT_TXN_OP_REF_DELETE):
    case (WT_TXN_OP_TRUNCATE_COL):
    case (WT_TXN_OP_TRUNCATE_ROW):
        return (false);
    case (WT_TXN_OP_BASIC_COL):
    case (WT_TXN_OP_BASIC_ROW):
    case (WT_TXN_OP_INMEM_COL):
    case (WT_TXN_OP_INMEM_ROW):
        return (true);
    }
    // WT_ASSERT_ALWAYS(session, false, "Unhandled op type encountered.");
    return (false);
}

/*
 * __txn_mod_compare --
 *     Qsort comparison routine for transaction modify list. Takes a session as a context argument.
 *     This allows for the use of comparators.
 */
static int WT_CDECL
__txn_mod_compare(const void *a, const void *b, void *context)
{
    WT_SESSION_IMPL *session;
    MockTxnOp *aopt, *bopt;
    int cmp;
    bool a_has_sortable_key;
    bool b_has_sortable_key;

    aopt = (MockTxnOp *)a;
    bopt = (MockTxnOp *)b;
    session = (WT_SESSION_IMPL *)context;

    /*
     * We want to sort on two things:
     *  - B-tree ID
     *  - Key
     * However, there are a number of modification types that don't have a key to be sorted on. This
     * requires us to add a stage between sorting on B-tree ID and key. At this intermediate stage,
     * we sort on whether the modifications have a key.
     *
     * We need to uphold the contract that all modifications on the same key are contiguous in the
     * final modification array. Technically they could be separated by non key modifications,
     * but for simplicity's sake we sort them apart.
     *
     * Qsort comparators are expected to return -1 if the first argument is smaller than the second,
     * 1 if the second argument is smaller than the first, and 0 if both arguments are equal.
     */

    /* Order by b-tree ID. */
    std::cout << "comparing item:" << aopt->op_identifer << "and" << bopt->op_identifer << std::endl;
    std::cout << "aopt btree id" << aopt->btree.id << std::endl;
    std::cout << "bopt btree id" << bopt->btree.id << std::endl;
    if (aopt->btree.id < bopt->btree.id)
        return (-1);
    if (aopt->btree.id > bopt->btree.id)
        return (1);

    /*
     * Order by whether the given operation has a key. We don't want to call key compare incorrectly
     * especially given that u is a union which would create undefined behavior.
     */
    a_has_sortable_key = __txn_mod_key_op(aopt);
    b_has_sortable_key = __txn_mod_key_op(bopt);
    std::cout << "a has sortable:" << a_has_sortable_key << "b has sortable:" << b_has_sortable_key << std::endl;
    if (a_has_sortable_key && !b_has_sortable_key)
        return (-1);
    if (b_has_sortable_key && !a_has_sortable_key)
        return (1);
    /*
     * In the case where both arguments don't have a key they are considered to be equal, we don't
     * care exactly how they get sorted.
     */
    if (!a_has_sortable_key && !b_has_sortable_key)
        return (0);

    std::cout << "aopt btree type" << aopt->btree.type << std::endl;

    /* Finally, order by key. Row-store requires a call to __wt_compare. */
    if (aopt->btree.type == BTREE_ROW) {
        std::cout << "comparing row entries" << std::endl;
        WT_ASSERT_ALWAYS(session,
          __wt_compare(
            session, aopt->btree.collator, aopt->op_row.item_key->get_item(), bopt->op_row.item_key->get_item(), &cmp) == 0,
          "Failed to sort transaction modifications during commit/rollback.");
        std::cout << "aopt key item" << (const uint8_t *) aopt->op_row.item_key->get_item()->data << std::endl;
        std::cout << "bopt key item" << (const uint8_t *) bopt->op_row.item_key->get_item()->data << std::endl;
        std::cout << "cmp:" << cmp << std::endl;
        return (cmp);
    }
    if (aopt->op_col.recno < bopt->op_col.recno){
        std::cout << "recno a < b" << aopt->op_col.recno << bopt->op_col.recno << std::endl;
        return (-1);
    }
    if (aopt->op_col.recno > bopt->op_col.recno){
        std::cout << "recno a > b" << aopt->op_col.recno << bopt->op_col.recno << std::endl;
        return (1);
    }
    return (0);
}

static bool WT_CDECL
__mod_ops_sorted(std::vector<MockTxnOp> input)
{
    // MockTxnOp aopt, bopt = MockTxnOp();
    int i;

    for(i=0; i < input.size()-1; i++){
        MockTxnOp aopt = input[i];
        MockTxnOp bopt = input[i+1];

        // /* A non-key'd operation cannot come before a key'd operation. */
        // if(__txn_mod_key_op(&bopt) && !(__txn_mod_key_op(&aopt)))
        //     return(false);

        // Non key'd operations can separate any modifications with keys.
        if((aopt.btree.id == bopt.btree.id) && (!__txn_mod_key_op(&bopt) || !__txn_mod_key_op(&aopt)))
            return (true);

        /* B-tree ids must be in ascending order.*/
        if((aopt.btree.id > bopt.btree.id) && __txn_mod_key_op(&bopt))
            return (false);
        
        /* Check the key/recno if btree ids are the same. */
        if(aopt.btree.id == bopt.btree.id){
            if (aopt.btree.type == BTREE_ROW) {
                WT_ITEM a_key = *aopt.op_row.item_key->get_item();
                auto a_data = a_key.data;

                WT_ITEM b_key = *aopt.op_row.item_key->get_item();
                auto b_data = b_key.data;

                if(a_data > b_data)
                    return (false);
            }

            if (aopt.op_col.recno > bopt.op_col.recno)
                return (false);
        }
    }
    return (true);
}

} // namespace


TEST_CASE("Basic cols and op_none", "[mod_compare]")
{
    MockTxnOp op1, op2, op3 = MockTxnOp();
    item_wrapper op1_key("1");
    std::vector<MockTxnOp> input{};

    // &op1->op_type = WT_TXN_OP_NONE;
    op1.set_optype(WT_TXN_OP_NONE);
    op1.set_btreeid(2);
    op1.set_opid(1);

    op1.op_row.item_key = &op1_key;

    /* WT_TXN_OP_REF_DELETE */
    op2.btree.type = BTREE_COL_VAR;
    op2.op_col.recno = 54;
    op2.set_btreeid(1);
    op2.set_optype(WT_TXN_OP_BASIC_COL);
    op2.set_opid(2);

    op3.set_btreeid(1);
    op3.btree.type = BTREE_COL_VAR;
    op3.op_col.recno = 60;
    op3.set_optype(WT_TXN_OP_BASIC_COL);
    op3.set_opid(3);

    input.push_back(op1);
    input.push_back(op2);
    input.push_back(op3);

    // Should be sorted 2->3->1
    int count = 0;
    __wt_qsort_r(&input[0], input.size(), sizeof(input[0]), __txn_mod_compare, &count);
    std::cout << "First element" << input[0].op_identifer;
    std::cout <<  "Second element" << input[1].op_identifer;
    std::cout <<  "Third element" << input[2].op_identifer;
    std::cout << std::endl;
    CHECK(__mod_ops_sorted(input));
}

TEST_CASE("Basic rows and op_none", "[mod_compare]")
{
    MockTxnOp op1, op2, op3, op4 = MockTxnOp();
    std::vector<MockTxnOp> input{};

    // &op1->op_type = WT_TXN_OP_NONE;
    op1.set_optype(WT_TXN_OP_NONE);
    // op1.btree->type = BTREE_
    op1.set_btreeid(1);
    op1.set_opid(1);

    /* WT_TXN_OP_REF_DELETE */
    op2.set_btreeid(1);
    op2.set_optype(WT_TXN_OP_BASIC_ROW);
    item_wrapper op2_key("5");
    op2.op_row.item_key = &op2_key;
    op2.set_opid(2);

    op3.set_btreeid(2);
    op3.set_optype(WT_TXN_OP_BASIC_ROW);
    item_wrapper op3_key("5");
    op3.op_row.item_key = &op3_key;
    op3.set_opid(3);

    op4.set_btreeid(2);
    op4.set_optype(WT_TXN_OP_BASIC_ROW);
    item_wrapper op4_key("1");
    op4.op_row.item_key = &op4_key;
    op4.set_opid(4);

    input.push_back(op1);
    input.push_back(op2);
    input.push_back(op3);
    input.push_back(op4);

    //expected output
    // op2, op4, op3, op1
    int count = 0;
    __wt_qsort_r(&input[0], input.size(), sizeof(input[0]), __txn_mod_compare, &count);

    std::cout << "First element" << input[0].op_identifer;
    std::cout <<  "Second element" << input[1].op_identifer;
    std::cout <<  "Third element" << input[2].op_identifer;
    std::cout <<  "Fourth element" << input[3].op_identifer;
    std::cout << std::endl;
    CHECK(__mod_ops_sorted(input));
}

TEST_CASE("Basic rows and op truncate col", "[mod_compare]")
{
    MockTxnOp op1, op2, op3, op4, op5 = MockTxnOp();
    std::vector<MockTxnOp> input{};

    // &op1->op_type = WT_TXN_OP_NONE;
    op1.set_optype(WT_TXN_OP_NONE);
    // op1.btree->type = BTREE_
    op1.set_btreeid(1);
    op1.set_opid(1);

    /* WT_TXN_OP_REF_DELETE */
    op2.set_btreeid(5);
    op2.set_optype(WT_TXN_OP_BASIC_ROW);
    item_wrapper op2_key("10");
    op2.op_row.item_key = &op2_key;
    op2.set_opid(2);

    op3.set_btreeid(5);
    op3.set_optype(WT_TXN_OP_BASIC_ROW);
    item_wrapper op3_key("8");
    op3.op_row.item_key = &op3_key;
    op3.set_opid(3);

    op4.set_btreeid(1);
    op4.set_optype(WT_TXN_OP_BASIC_ROW);
    item_wrapper op4_key("1");
    op4.op_row.item_key = &op4_key;
    op4.set_opid(4);

    op5.set_optype(WT_TXN_OP_TRUNCATE_COL);
    // op1.btree->type = BTREE_
    op5.set_btreeid(4);
    op5.set_opid(5);

    input.push_back(op1);
    input.push_back(op2);
    input.push_back(op3);
    input.push_back(op4);
    input.push_back(op5);

    //expected output
    // 4-> 1-> 2-> 3
    int count = 0;
    __wt_qsort_r(&input[0], input.size(), sizeof(input[0]), __txn_mod_compare, &count);

    std::cout << "First element" << input[0].op_identifer;
    std::cout <<  "Second element" << input[1].op_identifer;
    std::cout <<  "Third element" << input[2].op_identifer;
    std::cout <<  "Fourth element" << input[3].op_identifer;
    std::cout <<  "Fifth element" << input[4].op_identifer;
    std::cout << std::endl;
    CHECK(__mod_ops_sorted(input));
}

TEST_CASE("Basic cols and other non key'd ops", "[mod_compare]")
{
    MockTxnOp op1, op2, op3, op4 = MockTxnOp();
    std::vector<MockTxnOp> input{};

    // &op1->op_type = WT_TXN_OP_NONE;
    op1.set_optype(WT_TXN_OP_REF_DELETE);
    op1.set_btreeid(1);
    op1.set_opid(1);

    op2.set_optype(WT_TXN_OP_NONE);
    op2.set_btreeid(2);
    op2.set_opid(2);

    op3.set_btreeid(1);
    op3.set_optype(WT_TXN_OP_INMEM_COL);
    op3.btree.type = BTREE_COL_VAR;
    op3.op_col.recno = 10;
    op3.set_opid(3);

    op4.set_btreeid(1);
    // op4.btree.type = BTREE_COL;
    op4.set_optype(WT_TXN_OP_INMEM_COL);
    op4.btree.type = BTREE_COL_VAR;
    // op4.op_col.recno = 6;
    op4.op_col.recno = 6;
    op4.set_opid(4);

    input.push_back(op1);
    input.push_back(op2);
    input.push_back(op3);
    input.push_back(op4);

    //expected output
    // 4->3->1->2
    // 4 -> 3
    int count = 0;
    __wt_qsort_r(&input[0], input.size(), sizeof(input[0]), __txn_mod_compare, &count);

    std::cout << std::endl;
    std::cout << "First element" << input[0].op_identifer << std::endl;
    std::cout <<  "Second element" << input[1].op_identifer << std::endl;
    std::cout <<  "Third element" << input[2].op_identifer << std::endl;
    std::cout <<  "Fourth element" << input[3].op_identifer << std::endl;
    CHECK(__mod_ops_sorted(input));
}
