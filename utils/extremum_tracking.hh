/*
 * Copyright (C) 2018-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <limits>

namespace detail {

template<typename T, typename Comparator>
class extremum_tracker {
    T _default_value;
    bool _is_set = false;
    T _value;
public:
    explicit extremum_tracker(T default_value) {
        _default_value = default_value;
    }

    void update(T value) {
        if (!_is_set) {
            _value = value;
            _is_set = true;
        } else {
            if (Comparator{}(value,_value)) {
                _value = value;
            }
        }
    }

    void update(const extremum_tracker& other) {
        if (other._is_set) {
            update(other._value);
        }
    }

    T get() const {
        if (_is_set) {
            return _value;
        }
        return _default_value;
    }
};

} // namespace detail

template <typename T>
using min_tracker = detail::extremum_tracker<T, std::less<T>>;

template <typename T>
using max_tracker = detail::extremum_tracker<T, std::greater<T>>;

template <typename T>
class min_max_tracker {
    min_tracker<T> _min_tracker;
    max_tracker<T> _max_tracker;
public:
    min_max_tracker()
        : _min_tracker(std::numeric_limits<T>::min())
        , _max_tracker(std::numeric_limits<T>::max())
    {}

    min_max_tracker(T default_min, T default_max)
        : _min_tracker(default_min)
        , _max_tracker(default_max)
    {}

    void update(T value) {
        _min_tracker.update(value);
        _max_tracker.update(value);
    }

    void update(const min_max_tracker<T>& other) {
        _min_tracker.update(other._min_tracker);
        _max_tracker.update(other._max_tracker);
    }

    T min() const {
        return _min_tracker.get();
    }

    T max() const {
        return _max_tracker.get();
    }
};
