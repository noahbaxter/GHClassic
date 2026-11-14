#pragma once

// A file's bytes as a decoder reads them, from memory or read in as asked
// (custom/audio.cpp), and a decoder's place in them.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace gh2
{
    struct FileBytes
    {
        uint64_t size = 0u;
        // Up to `n` from `offset` into `dst`, how many there were.
        std::function<size_t(uint64_t offset, uint8_t *dst, size_t n)> read;
    };

    // Those bytes from `from` on.
    inline FileBytes heldBytes(std::shared_ptr<const std::vector<uint8_t>> bytes, size_t from = 0u)
    {
        const uint64_t size = bytes && from < bytes->size() ? bytes->size() - from : 0u;
        return {size, [bytes, from, size](uint64_t offset, uint8_t *dst, size_t n)
                {
                    n = static_cast<size_t>(std::min<uint64_t>(n, offset < size ? size - offset : 0u));
                    std::memcpy(dst, bytes->data() + from + offset, n);
                    return n;
                }};
    }

    // Read on from where the last read ended, as each decoder's callbacks do.
    struct FileCursor
    {
        FileBytes bytes;
        uint64_t at = 0u;

        size_t take(void *dst, size_t n)
        {
            n = bytes.read(at, static_cast<uint8_t *>(dst), n);
            at += n;
            return n;
        }
        // To `offset` from the start, here or the end (SEEK_SET, SEEK_CUR, SEEK_END).
        bool to(int64_t offset, int whence)
        {
            const int64_t base = whence == SEEK_CUR ? int64_t(at) : whence == SEEK_END ? int64_t(bytes.size) : 0;
            if (base + offset < 0 || base + offset > int64_t(bytes.size))
                return false;
            at = static_cast<uint64_t>(base + offset);
            return true;
        }
    };
}
