#ifndef RING_BUFFER_H__
#define RING_BUFFER_H__
#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <mutex>

#define platform_cache_line_size 64

// SPSC ringbuffer
class ringbuffer_spsc {
  public:
    ringbuffer_spsc(uint64_t buf_size, bool enable_cv)
        : buffer_(static_cast<uint8_t*>(malloc(buf_size)))
        , buf_size_(buf_size)
        , enable_cv_(enable_cv)
        , in_(0)
        , out_(0) {
        assert(buffer_);
    }

    ~ringbuffer_spsc() {
        free(buffer_);
    }

    const uint64_t get_total_in() const {
        return in_;
    }
    const uint64_t get_total_out() const {
        return out_;
    }

    void read(uint8_t* buf, uint64_t len) {
        if (enable_cv_) {
            {
                auto lk = std::unique_lock(mtx_);
                cv_.wait(lk, [this, len] {
                    return can_get(len);
                });

                get(buf, len);
            }

            cv_.notify_one();
        } else {
            while (!can_get(len)) {
            }
            get(buf, len);
        }
    }

    void write(const uint8_t* buf, uint64_t len) {
        if (len > buf_size_) {
            printf("write len %lu > buf_size_ %lu\n", len, buf_size_);
            assert(false);
        }
        if (enable_cv_) {
            {
                auto lk = std::unique_lock(mtx_);
                cv_.wait(lk, [this, len] {
                    return can_put(len);
                });

                put(buf, len);
            }

            cv_.notify_one();
        } else {
            while (!can_put(len)) {
            }
            put(buf, len);
        }
    }

  private:
    void put(const uint8_t* buf, uint64_t len) {
        uint64_t l = std::min(len, buf_size_ - (in_ & (buf_size_ - 1)));
        memcpy((void*)(buffer_ + (in_ & (buf_size_ - 1))), buf, l);
        memcpy((void*)buffer_, buf + l, len - l);
        std::atomic_thread_fence(std::memory_order_release);
        in_ += len;
    }

    void get(uint8_t* buf, uint64_t len) {
        uint64_t l = std::min(len, buf_size_ - (out_ & (buf_size_ - 1)));
        memcpy(buf, buffer_ + (out_ & (buf_size_ - 1)), l);
        memcpy(buf + l, buffer_, len - l);

        std::atomic_thread_fence(std::memory_order_release);
        out_ += len;
    }

    bool can_put(uint64_t size) const {
        std::atomic_thread_fence(std::memory_order_acquire);
        return in_ + size <= buf_size_ + out_;
    }

    bool can_get(uint64_t size) const {
        std::atomic_thread_fence(std::memory_order_acquire);
        return in_ >= size + out_;
    }

  private:
    uint8_t* const buffer_;
    const uint64_t buf_size_;
    const bool enable_cv_;
    std::mutex mtx_;
    std::condition_variable cv_;
    alignas(platform_cache_line_size) uint64_t in_;
    alignas(platform_cache_line_size) uint64_t out_;
};

// MPMC ringbuffer
class ringbuffer_mpmc {
  public:
    ringbuffer_mpmc(uint64_t buf_size, bool enable_cv)
        : buffer_(static_cast<uint8_t*>(malloc(buf_size)))
        , buf_size_(buf_size)
        , enable_cv_(enable_cv)
        , in_(0)
        , real_in_(0)
        , out_(0)
        , real_out_(0) {
        assert(buffer_);
    }

    ~ringbuffer_mpmc() {
        free(buffer_);
    }

    const uint64_t get_total_in() const {
        return real_in_.load(std::memory_order_acquire);
    }
    const uint64_t get_total_out() const {
        return real_out_.load(std::memory_order_acquire);
    }

    void read(uint8_t* buf, uint64_t len) {
        return get(buf, len);
    }

    void write(const uint8_t* buf, uint64_t len) {
        return put(buf, len);
    }

  private:
    void put(const uint8_t* buf, uint64_t len) {
        while (true) {
            if (enable_cv_) {
                auto lk = std::unique_lock(mtx_real_out_);
                cv_real_out_.wait(lk, [&] {
                    return in_.load(std::memory_order_relaxed) + len <= buf_size_ + real_out_.load(std::memory_order_acquire);
                });
            }

            uint64_t expected_in = in_.load(std::memory_order_relaxed);

            if (expected_in + len > buf_size_ + real_out_.load(std::memory_order_acquire) ||
                !in_.compare_exchange_weak(expected_in, expected_in + len, std::memory_order_relaxed, std::memory_order_relaxed)) {
                continue;
            }

            const uint64_t l = std::min(len, buf_size_ - (expected_in & (buf_size_ - 1)));
            // asm volatile("mfence" ::: "memory");
            memcpy((void*)(buffer_ + (expected_in & (buf_size_ - 1))), buf, l);
            memcpy((void*)buffer_, buf + l, len - l);
            // asm volatile("sfence" ::: "memory");

            const uint64_t expected_in_bak = expected_in;
            while (!real_in_.compare_exchange_weak(expected_in, expected_in + len, std::memory_order_release, std::memory_order_relaxed)) {
                expected_in = expected_in_bak;
            }

            if (enable_cv_) {
                cv_real_in_.notify_one();
            }

            break;
        }
    }

    void get(uint8_t* buf, const uint64_t len) {

        while (true) {
            if (enable_cv_) {
                auto lk = std::unique_lock(mtx_real_in_);
                cv_real_in_.wait(lk, [&] {
                    return real_in_.load(std::memory_order_acquire) >= out_.load(std::memory_order_relaxed) + len;
                });
            }

            uint64_t expected_out = out_.load(std::memory_order_relaxed);

            if (real_in_.load(std::memory_order_acquire) < expected_out + len ||
                !out_.compare_exchange_weak(expected_out, expected_out + len, std::memory_order_relaxed, std::memory_order_relaxed)) {
                continue;
            }

            const uint64_t l = std::min(len, buf_size_ - (expected_out & (buf_size_ - 1)));
            // asm volatile("lfence" ::: "memory");
            memcpy(buf, buffer_ + (expected_out & (buf_size_ - 1)), l);
            memcpy(buf + l, buffer_, len - l);
            // asm volatile("mfence" ::: "memory");

            const uint64_t expected_out_bak = expected_out;
            while (!real_out_.compare_exchange_weak(expected_out, expected_out + len, std::memory_order_release, std::memory_order_relaxed)) {
                expected_out = expected_out_bak;
            }

            if (enable_cv_) {
                cv_real_out_.notify_one();
            }

            break;
        }
    }

  private:
    uint8_t* const buffer_;
    const uint64_t buf_size_;
    const bool enable_cv_;

    alignas(platform_cache_line_size) std::atomic<uint64_t> in_;
    std::atomic<uint64_t> real_in_;
    std::mutex mtx_real_in_;
    std::condition_variable cv_real_in_;

    alignas(platform_cache_line_size) std::atomic<uint64_t> out_;
    std::atomic<uint64_t> real_out_;
    std::mutex mtx_real_out_;
    std::condition_variable cv_real_out_;
};

#endif