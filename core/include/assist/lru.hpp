// A fixed-capacity least-recently-used cache that never allocates after construction.
//
// The entries live in one vector and are linked into a recency list by index. A hash table finds an entry from its
// key: open addressing with linear probing, sized to at most half full, and deletion by shifting later entries back
// (Knuth, TAOCP vol. 3, algorithm 6.4R) so there are no tombstones to slow lookups down over time. get() and put()
// are O(1) on average. Once the cache is full an insert reuses the slot of the entry it evicts, and std::unordered_map
// is deliberately not used because it allocates a node for every insert. That matters on a device where an allocation
// in the middle of handling a request is both a latency spike and a place to run out of memory.
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace assist {

template <class Key, class Value, class Hash = std::hash<Key>>
class LruCache {
public:
    explicit LruCache(std::size_t capacity) : capacity_(capacity) {
        slots_.reserve(capacity);
        table_.assign(std::bit_ceil(std::max<std::size_t>(capacity * 2, 4)), kNone);
        mask_ = table_.size() - 1;
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size() const noexcept { return slots_.size(); }
    std::uint64_t hits() const noexcept { return hits_; }
    std::uint64_t misses() const noexcept { return misses_; }

    // The cached value, marking it as the most recently used. A pointer into the cache, valid until the next put().
    const Value* get(const Key& key) {
        const std::size_t h = Hash{}(key);
        const std::size_t pos = find(key, h);
        if (pos == kNotFound) {
            ++misses_;
            return nullptr;
        }
        ++hits_;
        const std::uint32_t slot = table_[pos];
        move_to_front(slot);
        return &slots_[slot].value;
    }

    void put(const Key& key, Value value) {
        if (capacity_ == 0) return;
        const std::size_t h = Hash{}(key);
        if (const std::size_t pos = find(key, h); pos != kNotFound) {
            const std::uint32_t slot = table_[pos];
            slots_[slot].value = std::move(value);
            move_to_front(slot);
            return;
        }
        std::uint32_t slot;
        if (slots_.size() < capacity_) {
            slot = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back(Slot{key, std::move(value), h, kNone, kNone});
        } else {
            slot = tail_;  // evict the least recently used entry and reuse its slot
            unlink(slot);
            erase_at(find(slots_[slot].key, slots_[slot].hash));
            slots_[slot].key = key;
            slots_[slot].value = std::move(value);
            slots_[slot].hash = h;
        }
        insert_at(slot, h);
        push_front(slot);
    }

    void clear() {
        slots_.clear();
        table_.assign(table_.size(), kNone);
        head_ = tail_ = kNone;
    }

private:
    static constexpr std::uint32_t kNone = 0xFFFFFFFFU;
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    struct Slot {
        Key key;
        Value value;
        std::size_t hash;
        std::uint32_t prev;
        std::uint32_t next;
    };

    std::size_t find(const Key& key, std::size_t h) const {
        std::size_t pos = h & mask_;
        while (true) {
            const std::uint32_t s = table_[pos];
            if (s == kNone) return kNotFound;
            if (slots_[s].hash == h && slots_[s].key == key) return pos;
            pos = (pos + 1) & mask_;
        }
    }

    void insert_at(std::uint32_t slot, std::size_t h) {
        std::size_t pos = h & mask_;
        while (table_[pos] != kNone) pos = (pos + 1) & mask_;
        table_[pos] = slot;
    }

    // Remove the entry at table position i, then walk forward and pull back every entry that would otherwise
    // become unreachable because its probe sequence passed through the gap.
    void erase_at(std::size_t i) {
        std::size_t j = i;
        while (true) {
            j = (j + 1) & mask_;
            if (table_[j] == kNone) break;
            const std::size_t ideal = slots_[table_[j]].hash & mask_;
            const bool stays = (i <= j) ? (i < ideal && ideal <= j) : (i < ideal || ideal <= j);
            if (stays) continue;
            table_[i] = table_[j];
            i = j;
        }
        table_[i] = kNone;
    }

    void unlink(std::uint32_t s) {
        Slot& x = slots_[s];
        if (x.prev != kNone) slots_[x.prev].next = x.next; else head_ = x.next;
        if (x.next != kNone) slots_[x.next].prev = x.prev; else tail_ = x.prev;
        x.prev = x.next = kNone;
    }

    void push_front(std::uint32_t s) {
        Slot& x = slots_[s];
        x.prev = kNone;
        x.next = head_;
        if (head_ != kNone) slots_[head_].prev = s;
        head_ = s;
        if (tail_ == kNone) tail_ = s;
    }

    void move_to_front(std::uint32_t s) {
        if (head_ == s) return;
        unlink(s);
        push_front(s);
    }

    std::size_t capacity_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> table_;
    std::size_t mask_ = 0;
    std::uint32_t head_ = kNone;
    std::uint32_t tail_ = kNone;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
};

}  // namespace assist
