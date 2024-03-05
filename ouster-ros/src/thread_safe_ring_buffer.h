/**
 * Copyright (c) 2018-2023, Ouster, Inc.
 * All rights reserved.
 *
 * @file thread_safe_ring_buffer.h
 * @brief File contains thread safe implementation of a ring buffer
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

/**
 * @class ThreadSafeRingBuffer thread safe ring buffer.
 */
class ThreadSafeRingBuffer {
   public:
    ThreadSafeRingBuffer(size_t item_size_, size_t items_count_)
        : buffer(item_size_ * items_count_),
          item_size(item_size_),
          max_items_count(items_count_),
          active_items_count(0),
          write_idx(SIZE_MAX),
          read_idx(SIZE_MAX),
          dropped_reads(0),
          should_always_drop_reads(true),
          new_data_lock(mutex, std::defer_lock),
          free_space_lock(mutex, std::defer_lock) {}

    /**
     * Gets the maximum number of items that this ring buffer can hold.
     */
    [[nodiscard]] size_t capacity() const { return max_items_count; }

    /**
     * Gets the number of item that currently occupy the ring buffer. This
     * number would vary between 0 and the capacity().
     *
     * @remarks
     *  if returned value was 0 or the value was equal to the buffer capacity(),
     *  this does not guarantee that a subsequent call to read() or write()
     *  wouldn't cause the calling thread to be blocked.
     */
    [[nodiscard]] size_t size() const { return active_items_count.load(); }

    /**
     * Checks if the ring buffer is empty.
     *
     * @remarks
     *  if empty() returns true this does not guarantee that calling the write()
     *  operation directly right after wouldn't block the calling thread.
     */
    [[nodiscard]] bool empty() const { return active_items_count.load() == 0; }

    /**
     * Checks if the ring buffer is full.
     *
     * @remarks
     *  if full() returns true this does not guarantee that calling the read()
     *  operation directly right after wouldn't block the calling thread.
     */
    [[nodiscard]] bool full() const {
        return active_items_count.load() == capacity();
    }

    /**
     * Writes to the buffer safely, this method will keep blocking until the
     * there is a space available within the buffer.
     */
    template <class BufferWriteFn>
    void write(BufferWriteFn&& buffer_write) {
        should_always_drop_reads = false;
        free_space_lock.lock();
        free_space_condition.wait(free_space_lock, [this] { return !full(); });
        free_space_lock.unlock();
        perform_write(buffer_write);
    }

    /**
     * Writes to the buffer safely, if there is no space left, then this method
     * will overwrite the last item.
     */
    template <class BufferWriteFn>
    void write_overwrite(BufferWriteFn&& buffer_write) {
        should_always_drop_reads = true;
        perform_write(buffer_write);
    }

    /**
     * Writes to the buffer safely, this method will return immediately and if
     * there is no space left, the data will not be written (will be dropped).
     */
    template <class BufferWriteFn>
    void write_nonblock(BufferWriteFn&& buffer_write) {
        should_always_drop_reads = false;
        if (!full()) perform_write(buffer_write);
    }

    /**
     * Gives access to read the buffer through a callback, this method will block
     * until there is something to read available.
     */
    template <typename BufferReadFn>
    void read(BufferReadFn&& buffer_read) {
        new_data_lock.lock();
        new_data_condition.wait(new_data_lock, [this] { return !empty(); });
        new_data_lock.unlock();
        perform_read(buffer_read);
    }

    /**
     * Gives access to read the buffer through a callback, if buffer is
     * inaccessible this method will timeout and the callback is not performed.
     */
    template <typename BufferReadFn>
    void read_timeout(BufferReadFn&& buffer_read,
                      std::chrono::seconds timeout) {
        new_data_lock.lock();
        if (new_data_condition.wait_for(
                new_data_lock, timeout, [this] { return !empty(); })) {
          new_data_lock.unlock();
          perform_read(buffer_read);
          return;
        }
        new_data_lock.unlock();
    }

    /**
     * Gives access to read the buffer through a callback, this method will return
     * immediately and the callback is performed only when there is data available.
     */
    template <typename BufferReadFn>
    void read_nonblock(BufferReadFn&& buffer_read) {
        if (!empty()) perform_read(buffer_read);
    }

  protected:
    /**
     * Resets the write_idx to an initial value.
     * @remarks
     *  Should be mostly used by tests to allow reading of the final item left
     *  in the buffer or restarting the test scenario.
     */
    void reset_write_idx() { write_idx = SIZE_MAX; }

    /**
     * Resets the read_idx to an initial value.
     * @remarks
     *  Should be mostly used by tests to allow restarting the test scenario.
     */
    void reset_read_idx() { read_idx = SIZE_MAX; }

    /**
     * Gets the max_allowed_read_drops value.
     * @return The statically set max allowed number of reading drops.
     * @remarks
     *  Should be mostly used by tests.
     */
    static constexpr uint32_t get_max_allowed_read_drops() {
      return MAX_ALLOWED_READ_DROPS;
    }

   private:
     /**
      * Performs the actual sequence of operations for writing.
      * @tparam BufferWriteFn
      * @param buffer_write
      */
     template <class BufferWriteFn>
     void perform_write(BufferWriteFn&& buffer_write) {
       buffer_write(&buffer[increment_with_capacity(write_idx) * item_size]);
       push();
       new_data_condition.notify_all();
     }

     /**
      * Performs the actual sequence of operations for reading.
      * @tparam BufferReadFn
      * @param buffer_read
      * @remarks
      *  If this function attempts to read using an index currently held by the
      *  writer, it will not perform the operations. However, if allowed, it will
      *  not keep dropping more than the MAX_ALLOWED_READ_DROPS, after which a
      *  single read is performed regardless.
      */
     template <typename BufferReadFn>
     void perform_read(BufferReadFn&& buffer_read) {
       if ((incremented_with_capacity(read_idx.load()) == write_idx.load())
           && (should_always_drop_reads.load() ||
               (dropped_reads.load() < MAX_ALLOWED_READ_DROPS))) {
         ++dropped_reads;
         return;
       }

       dropped_reads = 0;
       buffer_read(&buffer[increment_with_capacity(read_idx) * item_size]);
       pop();
       free_space_condition.notify_one();
     }

    /**
     * Atomically increments a given index, wrapping around with the buffer capacity.
     * Also returns the incremented value so that only a single atomic load is
     * performed for this operation.
     * @param idx Reference to the index to be incremented.
     * @return The new incremented value of the index.
     */
    [[nodiscard]] size_t increment_with_capacity(std::atomic_size_t &idx) const {
        const size_t incremented = (idx.load() + 1) % capacity();
        idx = incremented;
        return incremented;
    }

    /**
     * Returns an incremented value of the given index, wrapping around with the
     * buffer capacity. This function does not modify the given index.
     * @param idx Current index value.
     * @return Incremented value of the given index.
     */
    [[nodiscard]] size_t incremented_with_capacity(const size_t idx) const {
        return (idx + 1) % capacity();
    }

    /**
     * Atomically increments the buffer active elements count, clamping at the
     * buffer capacity.
     */
    void push() {
      active_items_count = std::min(active_items_count.load() + 1, capacity());
    }

    /**
     * Atomically decrements the buffer active elements count, clamping at zero.
     */
    void pop() {
      active_items_count = static_cast<size_t>(std::max(
          static_cast<int>(active_items_count.load() - 1), 0));
    }

    static constexpr uint32_t MAX_ALLOWED_READ_DROPS = UINT16_MAX * 6;

    std::vector<uint8_t> buffer;

    const size_t item_size;
    const size_t max_items_count;

    std::atomic_size_t active_items_count;
    std::atomic_size_t write_idx;
    std::atomic_size_t read_idx;

    std::atomic_uint32_t dropped_reads;
    std::atomic_bool should_always_drop_reads;

    std::mutex mutex;
    std::condition_variable new_data_condition;
    std::unique_lock<std::mutex> new_data_lock;
    std::condition_variable free_space_condition;
    std::unique_lock<std::mutex> free_space_lock;

    friend class ThreadSafeRingBufferTest;
};