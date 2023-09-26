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

#ifndef MODEL_DATA_VALUE_H
#define MODEL_DATA_VALUE_H

#include <optional>
#include <string>

#include "model/core.h"

namespace model {

/*
 * NONE_STRING --
 *     The "string" to print in place of NONE.
 */
extern const std::string NONE_STRING;

/*
 * data_value --
 *     The data value stored in the model used for keys and values. We use a generic class, rather
 *     than a specific type such as std::string, to give us flexibility to change data types in the
 *     future, e.g., if this becomes necessary to explore additional code paths. This class is
 *     intended to parallel WiredTiger's WT_ITEM, which supports multiple data types, plus the
 *     ability to specify a NONE value to simplify modeling deleted data.
 */
class data_value {

public:
    /*
     * data_value::data_value --
     *     Create a new instance.
     */
    inline data_value(const char *data) : _data(data) {}

    /*
     * data_value::data_value --
     *     Create a new instance.
     */
    inline data_value(const std::string &data) noexcept : _data(data) {}

    /*
     * data_value::data_value --
     *     Create a new instance.
     */
    inline data_value(const std::string &&data) noexcept : _data(std::move(data)) {}

    /*
     * data_value::create_none --
     *     Create an instance of a "None" value.
     */
    inline static data_value
    create_none() noexcept
    {
        return data_value();
    }

    /*
     * data_value::as_string --
     *     Return the data value as a human-readable string (e.g., for printing).
     */
    inline const std::string &
    as_string() const noexcept
    {
        return _data.has_value() ? *_data : NONE_STRING;
    }

    /*
     * data_value::operator== --
     *     Compare to another data value.
     */
    inline bool
    operator==(const data_value &other) const noexcept
    {
        return _data == other._data;
    }

    /*
     * data_value::operator!= --
     *     Compare to another data value.
     */
    inline bool
    operator!=(const data_value &other) const noexcept
    {
        return !(*this == other);
    }

    /*
     * data_value::operator< --
     *     Compare to another data value.
     */
    inline bool
    operator<(const data_value &other) const noexcept
    {
        return _data < other._data;
    }

    /*
     * data_value::operator<= --
     *     Compare to another data value.
     */
    inline bool
    operator<=(const data_value &other) const noexcept
    {
        return _data <= other._data;
    }

    /*
     * data_value::operator> --
     *     Compare to another data value.
     */
    inline bool
    operator>(const data_value &other) const noexcept
    {
        return !(*this <= other);
    }

    /*
     * data_value::operator> --
     *     Compare to another data value.
     */
    inline bool
    operator>=(const data_value &other) const noexcept
    {
        return !(*this < other);
    }

    /*
     * data_value::tombstone --
     *     Check if this is a None value.
     */
    inline bool
    none() const noexcept
    {
        return !_data.has_value();
    }

private:
    /*
     * data_value::data_value --
     *     Create a new instance.
     */
    inline data_value() : _data(std::nullopt) {}

    std::optional<std::string> _data;
};

/*
 * NONE --
 *     The "None" value.
 */
extern const data_value NONE;

} /* namespace model */
#endif
