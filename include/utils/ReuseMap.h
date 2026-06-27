#pragma once

#include <cstddef>
#include <vector>
#include <utility>

namespace usbipdcpp {

/**
 * @brief A std::vector-based map that marks slots as empty on deletion;
 *        empty slots are reused on the next insert, and push_back only expands when no empty slots remain.
 *        Linear lookup O(n), suitable for small-capacity, high-frequency insert/erase scenarios.
 */
template<typename Key, typename Value>
class ReuseMap {
    struct Slot {
        Key key{};
        Value value{};
        bool occupied = false;
    };

    std::vector<Slot> slots_;
    size_t size_ = 0;

public:
    /// Pre-allocate memory to avoid reallocations on subsequent push_backs
    void reserve(size_t n) { slots_.reserve(n); }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    Value *insert(const Key &key, Value value) {
        // Check if it already exists
        for (auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return &slot.value;
        }
        // Reuse an empty slot
        for (auto &slot : slots_) {
            if (!slot.occupied) {
                slot.key = key;
                slot.value = std::move(value);
                slot.occupied = true;
                size_++;
                return &slot.value;
            }
        }
        // No empty slots; expand
        slots_.push_back({key, std::move(value), true});
        size_++;
        return &slots_.back().value;
    }

    Value *find(const Key &key) {
        for (auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return &slot.value;
        }
        return nullptr;
    }

    const Value *find(const Key &key) const {
        for (const auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return &slot.value;
        }
        return nullptr;
    }

    bool erase(const Key &key) {
        for (auto &slot : slots_) {
            if (slot.occupied && slot.key == key) {
                slot.occupied = false;
                size_--;
                return true;
            }
        }
        return false;
    }

    template<typename F>
    void for_each(F &&f) {
        for (auto &slot : slots_) {
            if (slot.occupied) f(slot.key, slot.value);
        }
    }

    template<typename F>
    void for_each(F &&f) const {
        for (const auto &slot : slots_) {
            if (slot.occupied) f(slot.key, slot.value);
        }
    }

    void clear() {
        for (auto &slot : slots_) slot.occupied = false;
        size_ = 0;
    }
};

}  // namespace usbipdcpp