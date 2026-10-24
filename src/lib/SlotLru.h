#pragma once
#include <cstddef>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

namespace eb {

// Maps book ids to image-list slots [first_slot, first_slot + capacity). An entry touched during the current
// paint generation is never evicted, so the items on screen cannot thrash each other out.
class SlotLru {
public:
    SlotLru() = default;
    SlotLru(size_t capacity, int first_slot) : capacity_(capacity), first_slot_(first_slot) {}

    void reset(size_t capacity, int first_slot)
    {
        capacity_ = capacity;
        first_slot_ = first_slot;
        entries_.clear();
        map_.clear();
        used_now_ = 0;
    }

    void begin_generation() noexcept
    {
        ++generation_;
        used_now_ = 0;
    }

    [[nodiscard]] bool contains(const std::string& id) const { return map_.contains(id); }
    [[nodiscard]] size_t size() const noexcept { return entries_.size(); }
    // True when every slot is taken by an entry used in the current generation.
    [[nodiscard]] bool saturated() const noexcept { return capacity_ == 0 || (entries_.size() >= capacity_ && used_now_ >= entries_.size()); }

    // Slot of a stored id, marking it used in this generation.
    std::optional<int> touch(const std::string& id)
    {
        const auto it = map_.find(id);
        if (it == map_.end())
            return std::nullopt;
        entries_.splice(entries_.begin(), entries_, it->second);
        mark(*it->second);
        return it->second->slot;
    }

    // Slot to (re)fill for id: its own, a free one, or the least recently used entry not used in this generation.
    // nullopt when every slot is in use.
    std::optional<int> acquire(const std::string& id)
    {
        if (const auto own = touch(id))
            return own;
        if (capacity_ == 0)
            return std::nullopt;
        int slot = 0;
        if (entries_.size() < capacity_) {
            slot = first_slot_ + static_cast<int>(entries_.size());
        } else {
            if (entries_.back().generation == generation_)
                return std::nullopt;
            slot = entries_.back().slot;
            map_.erase(entries_.back().id);
            entries_.pop_back();
        }
        entries_.push_front({id, slot, 0});
        map_[id] = entries_.begin();
        mark(entries_.front());
        return slot;
    }

private:
    struct Entry {
        std::string id;
        int slot;
        size_t generation;
    };

    void mark(Entry& e) noexcept
    {
        if (e.generation != generation_) {
            e.generation = generation_;
            ++used_now_;
        }
    }

    size_t capacity_ = 0;
    int first_slot_ = 0;
    size_t generation_ = 1;
    size_t used_now_ = 0;
    std::list<Entry> entries_; // front = most recently used
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
};

}
