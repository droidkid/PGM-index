// This file is part of PGM-index <https://github.com/gvinciguerra/PGM-index>.
// Copyright (c) 2019 Giorgio Vinciguerra.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <queue>
#include <limits>
#include <vector>
#include <utility>
#include <numeric>
#include <cassert>
#include <iterator>
#include "pgm_index.hpp"

/**
 * A sorted associative container that contains key-value pairs with unique keys.
 * @tparam K the type of a key
 * @tparam V the type of a value
 * @tparam PGMType the type of @ref PGMIndex to use in the container
 * @tparam MinIndexedLevel the minimum level (of size 2^MinIndexedLevel) on which a @p PGMType index is constructed
 */
template<typename K, typename V, typename PGMType = PGMIndex<K>, uint8_t MinIndexedLevel = 18>
class DynamicPGMIndex {
    class Item;
    class BaseItemA;
    class BaseItemB;
    class DynamicPGMIndexIterator;

    template<typename T, typename A=std::allocator<T>>
    class DefaultInitAllocator;

    constexpr static uint8_t min_level = 6;    ///< 2^min_level-1 is the size of the sorted buffer for new items.
    constexpr static uint8_t init_levels = 15; ///< Number of levels to allocate when not bulk loaded.
    constexpr static uint8_t max_fully_allocated_level = std::max(15, min_level + 1);

    static_assert(min_level < MinIndexedLevel);
    static_assert(max_fully_allocated_level > min_level);
    static_assert(2 * PGMType::epsilon_value < 1ul << MinIndexedLevel);

    using LevelType = std::vector<Item, DefaultInitAllocator<Item>>;
    using BaseItem = std::conditional_t<std::is_pointer_v<V>, BaseItemA, BaseItemB>;

    uint8_t used_levels;         ///< Equal to 1 + last level whose size is greater than 0, or = min_level if no data.
    std::vector<LevelType> data; ///< (i-min_level)th element is the data array on the ith level.
    std::vector<PGMType> pgm;    ///< (i-MinIndexedLevel)th element is the index on the ith level.

    const LevelType &get_level(uint8_t level) const { return data[level - min_level]; }
    const PGMType &get_pgm(uint8_t level) const { return pgm[level - MinIndexedLevel]; }
    LevelType &get_level(uint8_t level) { return data[level - min_level]; }
    PGMType &get_pgm(uint8_t level) { return pgm[level - MinIndexedLevel]; }

    template<bool SkipDeleted, typename In1, typename In2, typename OutIterator>
    static OutIterator merge(In1 first1, In1 last1, In2 first2, In2 last2, OutIterator result) {
        while (first1 != last1 && first2 != last2) {
            if (*first2 < *first1) {
                *result = *first2;
                ++first2;
                ++result;
            } else if (*first1 < *first2) {
                *result = *first1;
                ++first1;
                ++result;
            } else if (SkipDeleted && first1->deleted()) {
                ++first1;
                ++first2;
            } else {
                *result = *first1;
                ++first1;
                ++first2;
                ++result;
            }
        }
        return std::copy(first2, last2, std::copy(first1, last1, result));
    }

    void pairwise_logarithmic_merge(const Item &new_item, uint8_t up_to_level,
                                    size_t size_hint, typename LevelType::iterator insertion_point) {
        const auto target_level = up_to_level + 1;
        auto &target_level_data = get_level(target_level);
        size_t actual_size = 1ull << (1 + min_level);
        assert((1ull << target_level) - target_level_data.size() >= actual_size);

        LevelType tmp_a(size_hint);
        LevelType tmp_b(size_hint + target_level_data.size()); // eventually tmp_b will replace target_level_data

        // Insert new item in sorted order in the first level
        const auto tmp1 = tmp_a.begin();
        const auto tmp2 = tmp_b.begin();
        const auto mod = (up_to_level - min_level) % 2;

        auto it = std::move(data[0].begin(), insertion_point, mod ? tmp1 : tmp2);
        *it = new_item;
        ++it;
        std::move(insertion_point, data[0].end(), it);

        // Merge subsequent levels
        uint8_t limit = target_level_data.empty() ? up_to_level : up_to_level + 1;
        for (uint8_t i = 1 + min_level; i <= limit; ++i) {
            bool alternate = (i - min_level) % 2 == mod;
            auto &level_data = get_level(i);
            auto tmp_it = alternate ? tmp1 : tmp2;
            auto out_it = alternate ? tmp2 : tmp1;
            decltype(out_it) out_end;

            bool can_delete_permanently = i == used_levels - 1;
            if (can_delete_permanently)
                out_end = merge<true>(tmp_it, tmp_it + actual_size, level_data.begin(), level_data.end(), out_it);
            else
                out_end = merge<false>(tmp_it, tmp_it + actual_size, level_data.begin(), level_data.end(), out_it);
            actual_size = std::distance(out_it, out_end);

            // Empty this level and the corresponding index
            level_data.clear();
            if (i > max_fully_allocated_level)
                level_data.shrink_to_fit();
            if (i >= MinIndexedLevel)
                get_pgm(i) = PGMType();
        }

        tmp_b.resize(actual_size);
        data[0].resize(0);
        data[target_level - min_level] = std::move(tmp_b);
        auto &new_target_data = get_level(target_level);

        // Rebuild index, if needed
        if (target_level >= MinIndexedLevel)
            get_pgm(target_level) = PGMType(new_target_data.begin(), new_target_data.end());
    }

    void insert(const Item &new_item) {
        auto insertion_point = std::lower_bound(data[0].begin(), data[0].end(), new_item);
        if (insertion_point != data[0].end() && *insertion_point == new_item) {
            *insertion_point = new_item;
            return;
        }

        size_t first_level_max_size = (1ull << (min_level + 1)) - 1;
        bool first_level_has_slots_left = data[0].size() < first_level_max_size;
        if (first_level_has_slots_left) {
            data[0].insert(insertion_point, new_item);
            used_levels = used_levels == min_level ? min_level + 1 : used_levels;
            return;
        }

        size_t slots_required = first_level_max_size + 1;
        uint8_t i;
        for (i = min_level + 1; i < used_levels; ++i) {
            size_t slots_left_in_level = (1ull << i) - get_level(i).size();
            if (slots_required <= slots_left_in_level)
                break;
            slots_required += get_level(i).size();
        }

        bool need_new_level = i == used_levels;
        if (need_new_level) {
            ++used_levels;
            data.emplace_back();
            if (i - MinIndexedLevel >= int(pgm.size()))
                pgm.emplace_back();
        }

        pairwise_logarithmic_merge(new_item, i - 1, slots_required, insertion_point);
    }

public:

    using key_type = K;
    using mapped_type = V;
    using value_type = Item;
    using size_type = size_t;
    using iterator = DynamicPGMIndexIterator;

    /**
     * Constructs an empty container.
     */
    DynamicPGMIndex() : used_levels(min_level), data(init_levels - min_level), pgm() {
        get_level(min_level).reserve((1ull << (min_level + 1)) - 1);
        for (uint8_t i = min_level + 1; i <= max_fully_allocated_level; ++i)
            get_level(i).reserve(1ull << i);
    }

    /**
     * Constructs the container on the sorted data in the range [first, last).
     * @tparam Iterator
     * @param first, last the range containing the sorted elements to be indexed
     */
    template<typename Iterator>
    DynamicPGMIndex(Iterator first, Iterator last) {
        assert(std::is_sorted(first, last));
        size_t n = std::distance(first, last);
        used_levels = std::ceil(std::log2(n)) + 1;
        data = decltype(data)(std::max(used_levels, max_fully_allocated_level) - min_level + 1);

        get_level(min_level).reserve((1ull << (min_level + 1)) - 1);
        for (uint8_t i = min_level + 1; i <= max_fully_allocated_level; ++i)
            get_level(i).reserve(1ull << i);

        // Copy only the first of each group of pairs with same key value
        auto &target = get_level(used_levels - 1);
        target.resize(n);
        auto out = target.begin();
        *out++ = Item(*first);
        while (++first != last)
            if (first->first != std::prev(out)->first)
                *out++ = Item(*first);
        target.resize(std::distance(target.begin(), out));

        if (used_levels - 1 >= MinIndexedLevel) {
            pgm = decltype(pgm)(used_levels - MinIndexedLevel);
            get_pgm(used_levels - 1) = PGMType(target.begin(), target.end());
        }
    }

    /**
     * Inserts an element into the container. If the container already contain an element with an equivalent key its
     * value is updated with @p value.
     * @param key element key to insert or update
     * @param value element value to insert
     */
    void insert(const K &key, const V &value) {
        insert(Item(key, value));
    }

    /**
     * Removes the specified element from the container.
     * @param key key value of the element to remove
     */
    void erase(const K &key) {
        insert(Item(key));
    }

    /**
     * Finds an element with key equivalent to @p key.
     * @param key key value of the element to search for
     * @return an iterator to an element with key equivalent to @p key. If no such element is found, end() is returned
     */
    iterator find(const K &key) const {
        uint8_t i = min_level;
        for (; i < std::min(MinIndexedLevel, used_levels); ++i) {
            auto &level = get_level(i);
            if (level.empty())
                continue;

            auto it = std::lower_bound(level.begin(), level.end(), key);
            if (it != level.end() && it->key() == key)
                return it->deleted() ? end() : iterator(this, it);
        }

        for (; i < used_levels; ++i) {
            auto &level = get_level(i);
            if (level.empty())
                continue;

            auto range = get_pgm(i).search(key);
            auto it = std::lower_bound(level.begin() + range.lo, level.begin() + range.hi, key);
            if (it != level.end() && it->key() == key)
                return it->deleted() ? end() : iterator(this, it);
        }

        return end();
    }

    /**
     * Returns an iterator pointing to the first element that is not less than (i.e. greater or equal to) @p key.
     * @param key key value to compare the elements to
     * @return an iterator to an element with key not less than @p key. If no such element is found, end() is returned
     */
    iterator lower_bound(const K &key) const {
        typename LevelType::const_iterator lo;
        bool lo_is_set = false;

        uint8_t i = min_level;
        for (; i < std::min(MinIndexedLevel, used_levels); ++i) {
            auto &level = get_level(i);
            if (level.empty())
                continue;

            auto it = std::lower_bound(level.begin(), level.end(), key);
            while (it != level.end() && it->deleted())
                ++it;

            if (it != level.end() && (it->key() >= key && (!lo_is_set || it->key() < lo->key()))) {
                lo = it;
                lo_is_set = true;
            }
        }

        for (; i < used_levels; ++i) {
            auto &level = get_level(i);
            if (level.empty())
                continue;

            auto range = get_pgm(i).search(key);
            auto it = std::lower_bound(level.begin() + range.lo, level.begin() + range.hi, key);
            while (it != level.end() && it->deleted())
                ++it;

            if (it != level.end() && (it->key() >= key && (!lo_is_set || it->key() < lo->key()))) {
                lo = it;
                lo_is_set = true;
            }
        }

        if (lo_is_set)
            return iterator(this, lo);

        return end();
    }

    /**
     * Checks if the container has no elements, i.e. whether begin() == end().
     * @return true if the container is empty, false otherwise
     */
    bool empty() const {
        return begin() == end();
    }

    /**
     * Returns an iterator to the beginning.
     * @return an iterator to the beginning
     */
    iterator begin() const { return lower_bound(std::numeric_limits<K>::min()); }

    /**
     * Returns an iterator to the end.
     * @return an iterator to the end
     */
    iterator end() const { return iterator(this, data.back().end()); }

    /**
     * Returns the number of elements with key that compares equal to the specified argument key, which is either 1
     * or 0 since this container does not allow duplicates.
     * @param key key value of the elements to count
     * @return number of elements with the given key, which is either 1 or 0.
     */
    size_t count(const K &key) const { return find(key) == end() ? 0 : 1; }

    /**
     * Returns the size of the container in bytes.
     * @return the size of the container in bytes
     */
    size_t size_in_bytes() const {
        size_t bytes = data.size() * sizeof(LevelType);
        for (auto &level_data: data)
            bytes += level_data.size() * sizeof(Item);
        return index_size_in_bytes() + bytes;
    }

    /**
     * Returns the size of the index used in this container in bytes.
     * @return the size of the index used in this container in bytes
     */
    size_t index_size_in_bytes() const {
        size_t index_size = 0;
        for (auto &p: pgm)
            index_size += p.size_in_bytes();
        return index_size;
    }

private:

    // from: https://stackoverflow.com/a/21028912
    template<typename T, typename A>
    class DefaultInitAllocator : public std::allocator<T> {
        typedef std::allocator_traits<A> a_t;

    public:
        template<typename U>
        struct rebind {
            using other = DefaultInitAllocator<U, typename a_t::template rebind_alloc<U> >;
        };

        using A::A;

        template<typename U>
        void construct(U *ptr) noexcept(std::is_nothrow_default_constructible_v<U>) {
            ::new(static_cast<void *>(ptr)) U;
        }

        template<typename U, typename...Args>
        void construct(U *ptr, Args &&... args) {
            a_t::construct(static_cast<A &>(*this), ptr, std::forward<Args>(args)...);
        }
    };
};

template<typename K, typename V, typename PGMType, uint8_t MinIndexedLevel>
class DynamicPGMIndex<K, V, PGMType, MinIndexedLevel>::DynamicPGMIndexIterator {
    friend class DynamicPGMIndex;

    using internal_iterator = typename LevelType::const_iterator;
    using queue_pair = std::pair<internal_iterator, int16_t>;
    using dynamic_pgm_type = DynamicPGMIndex<K, V, PGMType, MinIndexedLevel>;

    static bool queue_cmp(const queue_pair &e1, const queue_pair &e2) {
        return *e1.first > *e2.first || (*e1.first == *e2.first && e1.second > e2.second);
    }

    const dynamic_pgm_type *super;
    internal_iterator it;
    bool initialized;
    std::priority_queue<queue_pair, std::vector<queue_pair>, decltype(&queue_cmp)> queue;

    void lazy_initialize_queue() {
        if (initialized)
            return;

        std::vector<queue_pair> initial_pairs{};
        initial_pairs.reserve(super->used_levels - super->min_level);

        // For each level create and position an iterator to the first key > it->key()
        for (uint8_t i = super->min_level; i < super->used_levels; ++i) {
            auto &level = super->get_level(i);
            if (level.empty())
                continue;

            size_t lo = 0;
            size_t hi = level.size();
            if (i >= MinIndexedLevel) {
                auto range = super->get_pgm(i).search(it->key());
                lo = range.lo;
                hi = range.hi;
            }

            auto pos = std::upper_bound(level.begin() + lo, level.begin() + hi, *it);
            if (pos != level.end())
                initial_pairs.emplace_back(pos, i);
        }

        queue = decltype(queue)(&queue_cmp, initial_pairs);
        initialized = true;
    }

    void advance() {
        if (queue.empty()) {
            *this = super->end();
            return;
        }

        auto queue_step = [&] {
            auto[level_it, level_idx] = queue.top();
            queue.pop();
            if (std::next(level_it) != super->get_level(level_idx).end())
                queue.emplace(std::next(level_it), level_idx);
            return level_it;
        };

        internal_iterator tmp_it;
        do {
            tmp_it = queue_step();
            while (!queue.empty() && queue.top().first->key() == tmp_it->key())
                queue_step();
        } while (!queue.empty() && tmp_it->deleted());

        if (tmp_it->deleted())
            *this = super->end();
        else
            it = tmp_it;
    }

    DynamicPGMIndexIterator() = default;

    DynamicPGMIndexIterator(const dynamic_pgm_type *super, const internal_iterator it)
        : super(super), it(it), initialized(), queue() {};

public:

    using iterator_category = std::forward_iterator_tag;
    using value_type = const Item;
    using pointer = const Item *;
    using reference = const Item &;

    DynamicPGMIndexIterator &operator++() {
        lazy_initialize_queue();
        advance();
        return *this;
    }

    DynamicPGMIndexIterator operator++(int) {
        DynamicPGMIndexIterator i(it);
        ++i;
        return i;
    }

    reference operator*() const { return *it; }
    pointer operator->() const { return &*it; };
    bool operator==(const DynamicPGMIndexIterator &rhs) const { return it == rhs.it; }
    bool operator!=(const DynamicPGMIndexIterator &rhs) const { return it != rhs.it; }
};

#pragma pack(push, 1)

template<typename K, typename V, typename PGMType, uint8_t MinIndexedLevel>
class DynamicPGMIndex<K, V, PGMType, MinIndexedLevel>::BaseItemA {
protected:
    static V tombstone;
    V second;

    BaseItemA() = default;
    explicit BaseItemA(const V &value) noexcept: second(value) {}

    bool deleted() const { return second == tombstone; }
    void set_deleted() { second = tombstone; }
};

template<typename K, typename V, typename PGMType, uint8_t MinIndexedLevel>
V DynamicPGMIndex<K, V, PGMType, MinIndexedLevel>::BaseItemA::tombstone = new std::remove_pointer_t<V>();

template<typename K, typename V, typename PGMType, uint8_t MinIndexedLevel>
class DynamicPGMIndex<K, V, PGMType, MinIndexedLevel>::BaseItemB {
protected:
    V second;
    bool flag;

    BaseItemB() = default;
    explicit BaseItemB(const V &value) noexcept: second(value), flag(false) {}

    bool deleted() const { return flag; }
    void set_deleted() { flag = true; }
};

template<typename K, typename V, typename PGMType, uint8_t MinIndexedLevel>
class DynamicPGMIndex<K, V, PGMType, MinIndexedLevel>::Item : private BaseItem {
    friend class DynamicPGMIndex;
    K first;

    Item() noexcept = default;
    explicit Item(const K &key) noexcept: BaseItem(), first(key) { BaseItem::set_deleted(); };

    Item(const K &key, const V &value) noexcept: BaseItem(value), first(key) {}
    explicit Item(const std::pair<K, V> &p) noexcept: BaseItem(p.second), first(p.first) {}

    bool deleted() const { return BaseItem::deleted(); }
    void set_deleted() { BaseItem::set_deleted(); }

public:

    const K &key() const { return first; }
    const V &value() const { return BaseItem::second; }
    operator K() const { return first; }
};

#pragma pack(pop)