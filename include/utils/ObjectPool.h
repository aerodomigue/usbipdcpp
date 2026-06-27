#pragma once

#include <algorithm>
#include <mutex>
#include <type_traits>
#include <utility>

namespace usbipdcpp {

namespace detail {
    template<typename T>
    struct DefaultLM {
        static T *create() {
            return new T{};
        }
        static void destroy(T *p) {
            delete p;
        }
    };

    template<typename T>
    struct DefaultReset {
        static void reset(T &) {
        }
    };
} // namespace detail

/**
 * @brief Fixed-size object pool
 *
 * Features:
 * - Pre-allocates all objects to avoid runtime memory allocation
 * - Can verify pointer ownership to prevent double-free
 * - alloc O(1), free O(log n)
 * - Optionally thread-safe
 * - Supports custom LifeManager and Reset strategies
 *
 * @warning **Reset is called at alloc() time**: an object's state is not cleared immediately after being returned;
 * Reset::reset() is only called on the next alloc(). If an object holds external resources (handles, locks, reference counts, etc.),
 * the caller must release those resources before calling free(), otherwise the resource lifetime will be deferred until the next alloc.
 * The Reset strategy is only responsible for resetting value fields of the object, not for releasing resources.
 *
 * @tparam T Object type
 * @tparam PoolSize Pool size
 * @tparam ThreadSafe Whether thread-safe (default false)
 * @tparam LifeManager Lifecycle manager providing create() / destroy(T*)
 * @tparam Reset Reset strategy; calls reset(T&, args...) on alloc; default is a no-op
 */
template<typename T, size_t PoolSize, bool ThreadSafe = false, typename LifeManager = detail::DefaultLM<T>,
         typename Reset = detail::DefaultReset<T>>
class ObjectPool {
    static_assert(PoolSize > 0, "PoolSize must be greater than 0");

public:
    ObjectPool() {
        // Create objects
        for (size_t i = 0; i < PoolSize; ++i) {
            pool_[i] = {LifeManager::create(), false};
        }
        // Sort by pointer for binary search
        std::sort(pool_, pool_ + PoolSize, [](const auto &a, const auto &b) { return a.first < b.first; });
        // Initialize the free index stack
        for (size_t i = 0; i < PoolSize; ++i) {
            free_stack_[i] = i;
        }
        free_top_ = PoolSize;
    }

    ~ObjectPool() {
        clear();
    }

    // Disallow copy and move
    ObjectPool(const ObjectPool &) = delete;
    ObjectPool &operator=(const ObjectPool &) = delete;
    ObjectPool(ObjectPool &&) = delete;
    ObjectPool &operator=(ObjectPool &&) = delete;

    /**
     * @brief Allocate an object
     * @param args Arguments passed to Reset::reset(T&, args...)
     * @return Pointer to the object; returns nullptr if the pool is empty, in which case the caller should fall back to LifeManager::create()
     *
     * Typical usage:
     * @code
     * auto* p = pool.alloc();
     * if (!p) p = LifeManager::create();
     * @endcode
     */
    template<typename... Args>
    T *alloc(Args &&...args) {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return alloc_impl(std::forward<Args>(args)...);
        }
        else {
            return alloc_impl(std::forward<Args>(args)...);
        }
    }

    /**
     * @brief Return an object to the pool
     * @param obj Object pointer
     * @return true if successfully returned; false if the pointer does not belong to this pool (e.g. a fallback create from alloc); caller should fall back to LifeManager::destroy()
     *
     * Typical usage:
     * @code
     * if (!pool.free(p)) LifeManager::destroy(p);
     * @endcode
     */
    bool free(T *obj) {
        if (!obj) {
            return false;
        }

        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return free_impl(obj);
        }
        else {
            return free_impl(obj);
        }
    }

    /**
     * @brief Get the number of available objects in the pool
     */
    size_t available() const {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return free_top_;
        }
        else {
            return free_top_;
        }
    }

    /**
     * @brief Get the total capacity of the pool
     */
    constexpr size_t capacity() const {
        return PoolSize;
    }

    /**
     * @brief Clear the pool (destroy all objects)
     */
    void clear() {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            clear_impl();
        }
        else {
            clear_impl();
        }
    }

    /**
     * @brief Reset the pool (return all objects to the pool without deleting them)
     */
    void reset() {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            reset_impl();
        }
        else {
            reset_impl();
        }
    }

private:
    std::pair<T *, bool> pool_[PoolSize] = {};
    size_t free_stack_[PoolSize] = {};
    size_t free_top_ = 0;
    mutable std::conditional_t<ThreadSafe, std::mutex, char> mutex_{};

    template<typename... Args>
    T *alloc_impl(Args &&...args) {
        if (free_top_ == 0) {
            return nullptr;
        }
        size_t index = free_stack_[--free_top_];
        pool_[index].second = true;
        T *obj = pool_[index].first;
        Reset::reset(*obj, std::forward<Args>(args)...);
        return obj;
    }

    bool free_impl(T *obj) {
        // Binary search for the pointer
        auto it = std::lower_bound(pool_, pool_ + PoolSize, obj,
                                   [](const auto &elem, T *val) { return elem.first < val; });
        if (it == pool_ + PoolSize || it->first != obj) {
            return false; // Not an object belonging to this pool
        }
        if (!it->second) {
            return false; // Double free
        }
        it->second = false;
        // Calculate index and push back onto the stack
        size_t index = it - pool_;
        free_stack_[free_top_++] = index;
        return true;
    }

    void clear_impl() {
        for (size_t i = 0; i < PoolSize; ++i) {
            if (pool_[i].first) {
                LifeManager::destroy(pool_[i].first);
                pool_[i] = {nullptr, false};
            }
        }
        free_top_ = 0;
    }

    void reset_impl() {
        // Return all objects to the pool without deleting
        for (size_t i = 0; i < PoolSize; ++i) {
            pool_[i].second = false;
        }
        // Rebuild the free index stack
        for (size_t i = 0; i < PoolSize; ++i) {
            free_stack_[i] = i;
        }
        free_top_ = PoolSize;
    }
};

} // namespace usbipdcpp
