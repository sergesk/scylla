/*
 * Copyright (C) 2020-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <boost/intrusive/list.hpp>
#include <seastar/core/memory.hh>

class evictable {
    friend class lru;
    using lru_link_type = boost::intrusive::list_member_hook<
        boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;
    lru_link_type _lru_link;
protected:
    // Prevent destruction via evictable pointer. LRU is not aware of allocation strategy.
    ~evictable() = default;
public:
    evictable() = default;
    evictable(evictable&& o) noexcept;
    evictable& operator=(evictable&&) noexcept = default;

    virtual void on_evicted() noexcept = 0;

    bool is_linked() const {
        return _lru_link.is_linked();
    }

    void unlink_from_lru() {
        _lru_link.unlink();
    }

    void swap(evictable& o) noexcept {
        _lru_link.swap_nodes(o._lru_link);
    }
};

class lru {
private:
    friend class evictable;
    using lru_type = boost::intrusive::list<evictable,
        boost::intrusive::member_hook<evictable, evictable::lru_link_type, &evictable::_lru_link>,
        boost::intrusive::constant_time_size<false>>; // we need this to have bi::auto_unlink on hooks.
    lru_type _list;
public:
    using reclaiming_result = seastar::memory::reclaiming_result;

    ~lru() {
        _list.clear_and_dispose([] (evictable* e) {
            e->on_evicted();
        });
    }

    void remove(evictable& e) noexcept {
        _list.erase(_list.iterator_to(e));
    }

    void add(evictable& e) noexcept {
        _list.push_back(e);
    }

    void touch(evictable& e) noexcept {
        remove(e);
        add(e);
    }

    // Evicts a single element from the LRU
    reclaiming_result evict() noexcept {
        if (_list.empty()) {
            return reclaiming_result::reclaimed_nothing;
        }
        evictable& e = _list.front();
        _list.pop_front();
        e.on_evicted();
        return reclaiming_result::reclaimed_something;
    }

    // Evicts all elements.
    // May stall the reactor, use only in tests.
    void evict_all() {
        while (evict() == reclaiming_result::reclaimed_something) {}
    }
};

inline
evictable::evictable(evictable&& o) noexcept {
    if (o._lru_link.is_linked()) {
        auto prev = o._lru_link.prev_;
        o._lru_link.unlink();
        lru::lru_type::node_algorithms::link_after(prev, _lru_link.this_ptr());
    }
}
