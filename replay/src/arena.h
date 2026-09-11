// A bump allocator for decoded data: create infos, command arguments and parsed JSON live in one
// until it is reset or destroyed. Everything it hands out is zeroed.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

namespace vkreplay {

class Arena {
public:
    explicit Arena(size_t chunkSize = 1 << 20) : _chunkSize(chunkSize) {}
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* Alloc(size_t size, size_t align = alignof(std::max_align_t)) {
        if (size == 0) size = 1;
        size_t pos = (_used + align - 1) & ~(align - 1);
        if (_blocks.empty() || pos + size > _capacity) {
            size_t capacity = std::max(_chunkSize, size);
            _blocks.push_back(std::make_unique<char[]>(capacity));  // value-initialized: zeroed
            _capacity = capacity;
            pos = 0;
        } else {
            std::memset(_blocks.back().get() + pos, 0, size);
        }
        _used = pos + size;
        return _blocks.back().get() + pos;
    }

    template <typename T>
    T* Make(size_t count = 1) {
        return static_cast<T*>(Alloc(sizeof(T) * std::max<size_t>(count, 1), alignof(T)));
    }

    void Reset() {
        _blocks.clear();
        _used = _capacity = 0;
    }

private:
    size_t _chunkSize;
    std::vector<std::unique_ptr<char[]>> _blocks;
    size_t _used = 0;
    size_t _capacity = 0;
};

} // namespace vkreplay
