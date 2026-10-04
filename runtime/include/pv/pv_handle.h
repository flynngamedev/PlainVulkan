// pv_handle.h -- the generic incrementing-id handle table shared by every
// subsystem that hands a resource id back to script code. Extracted from
// pv_internal.h so the scene, animation, particle, and physics headers can
// each declare their own tables without any of them pulling in Vulkan.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pv {

// Incrementing ids, so removing one resource never shifts anyone else's
// handle the way a plain index into a vector would. Id 0 is permanently
// reserved as "invalid", which is what lets a default-constructed uint64_t
// field (mesh = 0, texture = 0) mean "unset" without a separate flag.
template <typename T>
class HandleTable {
public:
    uint64_t add(T value) {
        uint64_t id = next_++;
        items_.emplace(id, std::move(value));
        return id;
    }
    T* get(uint64_t id) {
        auto it = items_.find(id);
        return it == items_.end() ? nullptr : &it->second;
    }
    const T* get(uint64_t id) const {
        auto it = items_.find(id);
        return it == items_.end() ? nullptr : &it->second;
    }
    void remove(uint64_t id) { items_.erase(id); }

    auto begin() { return items_.begin(); }
    auto end() { return items_.end(); }
    auto begin() const { return items_.begin(); }
    auto end() const { return items_.end(); }

    // Stable snapshot of every live id. Iterating a HandleTable directly
    // while adding or removing entries invalidates the underlying
    // unordered_map iterators -- which is exactly what a physics step
    // (bodies destroyed on contact) or a scene update (entities spawned by
    // script) does. Callers that mutate during iteration use this instead.
    std::vector<uint64_t> ids() const {
        std::vector<uint64_t> out;
        out.reserve(items_.size());
        for (const auto& kv : items_) out.push_back(kv.first);
        return out;
    }

    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }

    // Deliberately does *not* reset next_: handles stay unique for the
    // lifetime of the process, so a stale handle held by script code after
    // a scene unload reads as invalid rather than silently aliasing a
    // freshly created resource.
    void clear() { items_.clear(); }

private:
    std::unordered_map<uint64_t, T> items_;
    uint64_t next_ = 1; // 0 is reserved as "invalid handle"
};

} // namespace pv
