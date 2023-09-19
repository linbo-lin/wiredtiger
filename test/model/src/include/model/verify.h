/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef MODEL_VERIFY_H
#define MODEL_VERIFY_H

#include "model/core.h"
#include "wiredtiger.h"

namespace model {

class kv_table;

/*
 * kv_table_verify_cursor --
 *     The verification cursor
 */
class kv_table_verify_cursor {

public:
    /*
     * kv_table_verify_cursor::kv_table_verify_cursor --
     *     Create a new instance of the verification cursor.
     */
    inline kv_table_verify_cursor(std::map<data_value, kv_item> &data) noexcept
        : _count(0), _data(data), _iterator(_data.begin())
    {
    }

    /*
     * kv_table_verify_cursor::has_next --
     *     Determine whether the cursor has a next value.
     */
    bool has_next();

    /*
     * kv_table_verify_cursor::verify_next --
     *     Verify the next key-value pair. This method is not thread-safe.
     */
    bool verify_next(const data_value &key, const data_value &value);

private:
    uint64_t _count;
    std::map<data_value, kv_item> &_data;
    std::map<data_value, kv_item>::iterator _iterator;
};

/*
 * kv_table_verifier --
 *     Table verification.
 */
class kv_table_verifier {

public:
    /*
     * kv_table_verifier::kv_table_verifier --
     *     Create a new instance of the verifier.
     */
    inline kv_table_verifier(kv_table &table) noexcept : _table(table), _verbose(false) {}

    /*
     * kv_table_verifier::verify --
     *     Verify the table by comparing a WiredTiger table against the model.
     */
    bool verify(WT_CONNECTION *connection);

private:
    kv_table &_table;
    bool _verbose;
};

} /* namespace model */
#endif
