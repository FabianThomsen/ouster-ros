#include <thread>
#include <cstring>
#include <random>
#include <gtest/gtest.h>
#include "../src/thread_safe_ring_buffer.h"

using namespace std::chrono_literals;

class ThreadSafeRingBufferTest : public ::testing::Test {
  protected:
    static constexpr int ITEM_SIZE = 4;   // predefined size for all items used in
    static constexpr int ITEM_COUNT = 3;  // number of item the buffer could hold

    void SetUp() override {
        buffer = std::make_unique<ThreadSafeRingBuffer>(ITEM_SIZE, ITEM_COUNT);
    }

    void TearDown() override {
        buffer.reset();
    }

    std::string rand_str(int size) {
      const std::string characters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<int> dist(0, characters.size() - 1);

      std::string result;
      for (int i = 0; i < size; ++i) {
          result += characters[dist(gen)];
      }
      return result;
    }

    std::vector<std::string> rand_vector_str(int vec_size, int str_size) {
      std::vector<std::string> output(vec_size);
      for (auto i = 0; i < vec_size; ++i)
        output[i] = rand_str(str_size);
      return output;
    }

    std::vector<std::string> known_vector_str(int vec_size, const std::string& known) {
      std::vector<std::string> output(vec_size);
      for (auto i = 0; i < vec_size; ++i)
        output[i] = known;
      return output;
    }

    void reset_writing() { buffer->reset_write_idx(); }

    std::unique_ptr<ThreadSafeRingBuffer> buffer;
};

TEST_F(ThreadSafeRingBufferTest, ReadWriteToBufferSimple) {

    assert (ITEM_COUNT > 1 && "or this test can't run");

    const int TOTAL_ITEMS = 10; // total items to process
    const std::vector<std::string> source = rand_vector_str(TOTAL_ITEMS, ITEM_SIZE);
    std::vector<std::string> target = known_vector_str(TOTAL_ITEMS, "0000");

    EXPECT_TRUE(buffer->empty());
    EXPECT_FALSE(buffer->full());

    for (int i = 0; i < ITEM_COUNT; ++i) {
      buffer->write([i, &source](uint8_t* buffer) {
          std::memcpy(buffer, &source[i][0], ITEM_SIZE);
      });
    }

    EXPECT_FALSE(buffer->empty());
    EXPECT_TRUE(buffer->full());

    // remove one item
    buffer->read([&target](uint8_t* buffer){
        std::memcpy(&target[0][0], buffer, ITEM_SIZE);
    });

    EXPECT_FALSE(buffer->empty());
    EXPECT_FALSE(buffer->full());
    EXPECT_EQ(buffer->size(), static_cast<size_t>(ITEM_COUNT - 1));

    // Due to the lock-free implementation, that last item would not be read, since
    // the reader can not know if it's still being written to. So we have to reset
    // the write index before reading out the buffer.
    reset_writing();
    for (int i = 1; i < ITEM_COUNT; ++i) {
      buffer->read([i, &target](uint8_t* buffer){
          std::memcpy(&target[i][0], buffer, ITEM_SIZE);
      });
    }

    EXPECT_TRUE(buffer->empty());
    EXPECT_FALSE(buffer->full());

    for (int i = 0; i < ITEM_COUNT; ++i) {
        std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
        EXPECT_EQ(target[i], source[i]); 
    }
}

TEST_F(ThreadSafeRingBufferTest, ReadWriteToBufferBlocking) {

    const int TOTAL_ITEMS = 10; // total items to process
    const std::vector<std::string> source = rand_vector_str(TOTAL_ITEMS, ITEM_SIZE);
    std::vector<std::string> target = known_vector_str(TOTAL_ITEMS, "0000");

    EXPECT_TRUE(buffer->empty());
    EXPECT_FALSE(buffer->full());

    std::thread producer([this, &source]() {
        for (int i = 0; i < TOTAL_ITEMS; ++i) {
            buffer->write([i, &source](uint8_t* buffer){
                std::memcpy(buffer, &source[i][0], ITEM_SIZE);
            });
        }
    });

    std::thread consumer([this, &target]() {
        int i = 0;
        while (i < TOTAL_ITEMS - 1) {
            buffer->read([&i, &target](uint8_t* buffer){
                std::memcpy(&target[i++][0], buffer, ITEM_SIZE);
            });
        }
        // Due to the lock-free implementation, that last item would not be read, since
        // the reader can not know if it's still being written to. So we have to reset
        // the write index before reading out the buffer.
        reset_writing();
        buffer->read([&i, &target](uint8_t* buffer){
          std::memcpy(&target[i++][0], buffer, ITEM_SIZE);
        });
    });

    producer.join();
    consumer.join();

    for (int i = 0; i < TOTAL_ITEMS; ++i) {
        std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
        EXPECT_EQ(target[i], source[i]);
    }
}

TEST_F(ThreadSafeRingBufferTest, ReadWriteToBufferWithOverwrite) {

    const int TOTAL_ITEMS = 10; // total items to process
    const std::vector<std::string> source = rand_vector_str(TOTAL_ITEMS, ITEM_SIZE);
    std::vector<std::string> target = known_vector_str(TOTAL_ITEMS, "0000");

    EXPECT_TRUE(buffer->empty());
    EXPECT_FALSE(buffer->full());

    std::thread producer([this, &source]() {
        for (int i = 0; i < TOTAL_ITEMS; ++i) {
            buffer->write_overwrite([i, &source](uint8_t* buffer){
                std::memcpy(buffer, &source[i][0], ITEM_SIZE);
            });
        }
    });

    // wait for 1 second before starting the consumer thread
    // allowing sufficient time for the producer thread to be
    // completely done
    std::this_thread::sleep_for(1s);
    // Due to the lock-free implementation, that last item would not be read, since
    // the reader can not know if it's still being written to. So we have to reset
    // the write index before reading out the buffer.
    reset_writing();
    std::thread consumer([this, &target]() {
        for (int i = 0; i < TOTAL_ITEMS; ++i) {
            buffer->read_timeout([i, &target](uint8_t* buffer){
                  std::memcpy(&target[i][0], buffer, ITEM_SIZE);
            }, 1s);
        }
    });

    producer.join();
    consumer.join();

    // Since our buffer can host only up to ITEM_COUNT simultaneously only the
    // last ITEM_COUNT items would have remained in the buffer by the time
    // the consumer started processing.
    // If TOTAL_ITEMS is not divisible by ITEM_COUNT, the beginning of the buffer,
    // will contain a section of ITEM_COUNT items with the latest overwritten data.
    for (int i = 0; i < TOTAL_ITEMS % ITEM_COUNT; ++i) {
      std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
      EXPECT_EQ(target[i], source[TOTAL_ITEMS - (TOTAL_ITEMS % ITEM_COUNT) + i]);
    }
    // If TOTAL_ITEMS is divisible by ITEM_COUNT, the whole buffer will contain
    // exactly the last ITEM_COUNT items. Otherwise, the end of the buffer will
    // contain a section of ITEM_COUNT items with older data.
    for (int i = TOTAL_ITEMS % ITEM_COUNT; i < ITEM_COUNT; ++i) {
      std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
      EXPECT_EQ(target[i], source[TOTAL_ITEMS - (TOTAL_ITEMS % ITEM_COUNT) - ITEM_COUNT + i]);
    }
    // The remaining part of the target will not have any new data, since the buffer,
    // will now be completely read out.
    for (int i = ITEM_COUNT; i < TOTAL_ITEMS; ++i) {
      std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
      EXPECT_EQ(target[i], "0000");
    }
}

TEST_F(ThreadSafeRingBufferTest, ReadWriteToBufferNonblocking) {

  const int TOTAL_ITEMS = 10; // total items to process
  const std::vector<std::string> source = rand_vector_str(TOTAL_ITEMS, ITEM_SIZE);
  std::vector<std::string> target = known_vector_str(TOTAL_ITEMS, "0000");

  EXPECT_TRUE(buffer->empty());
  EXPECT_FALSE(buffer->full());

  std::thread producer([this, &source]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->write_nonblock([i, &source](uint8_t* buffer){
        std::memcpy(buffer, &source[i][0], ITEM_SIZE);
      });
    }
  });

  // wait for 1 second before starting the consumer thread
  // allowing sufficient time for the producer thread to be
  // completely done
  std::this_thread::sleep_for(1s);
  // Due to the lock-free implementation, that last item would not be read, since
  // the reader can not know if it's still being written to. So we have to reset
  // the write index before reading out the buffer.
  reset_writing();
  std::thread consumer([this, &target]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->read_nonblock([i, &target](uint8_t* buffer){
        std::memcpy(&target[i][0], buffer, ITEM_SIZE);
      });
    }
  });

  producer.join();
  consumer.join();

  // Since our buffer can host only up to ITEM_COUNT simultaneously only the
  // first ITEM_COUNT items will be written into the buffer, with the rest being
  // ignored.
  for (int i = 0; i < ITEM_COUNT; ++i) {
    std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
    EXPECT_EQ(target[i], source[i]);
  }
  // The remaining part of the target will not have any new data, since the buffer,
  // will now be completely read out.
  for (int i = ITEM_COUNT; i < TOTAL_ITEMS; ++i) {
    std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
    EXPECT_EQ(target[i], "0000");
  }
}

TEST_F(ThreadSafeRingBufferTest, ReadWriteToBufferBlockingThrottling) {

  const int TOTAL_ITEMS = 10; // total items to process
  const std::vector<std::string> source = rand_vector_str(TOTAL_ITEMS, ITEM_SIZE);
  std::vector<std::string> target = known_vector_str(TOTAL_ITEMS, "0000");
  static constexpr std::chrono::milliseconds period(10);

  EXPECT_TRUE(buffer->empty());
  EXPECT_FALSE(buffer->full());

  // First, the producer will write to the buffer faster than the consumer can read.
  std::thread faster_producer([this, &source]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->write([i, &source](uint8_t* buffer){
        std::memcpy(buffer, &source[i][0], ITEM_SIZE);
      });
      std::this_thread::sleep_for(period);
    }
  });

  std::thread slower_consumer([this, &target]() {
    int i = 0;
    while (i < TOTAL_ITEMS - 1) {
      buffer->read([&i, &target](uint8_t* buffer){
        std::memcpy(&target[i++][0], buffer, ITEM_SIZE);
      });
      std::this_thread::sleep_for(period * 4);
    }

    // Due to the lock-free implementation, that last item would not be read, since
    // the reader can not know if it's still being written to. So we have to reset
    // the write index before reading out the buffer.
    reset_writing();
    buffer->read([&i, &target](uint8_t* buffer){
      std::memcpy(&target[i++][0], buffer, ITEM_SIZE);
    });
  });

  faster_producer.join();
  slower_consumer.join();

  ASSERT_TRUE(buffer->empty());
  ASSERT_FALSE(buffer->full());

  // Blocking read and write should be synchronized even if one thread is faster.
  std::cout << "Faster producer, slower consumer:" << std::endl;
  for (int i = 0; i < TOTAL_ITEMS; ++i) {
    std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
    EXPECT_EQ(target[i], source[i]);
  }

  target = known_vector_str(TOTAL_ITEMS, "0000");

  // Then, then consumer will read faster than the producer can write.
  std::thread slower_producer([this, &source]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->write([i, &source](uint8_t* buffer){
        std::memcpy(buffer, &source[i][0], ITEM_SIZE);
      });
      std::this_thread::sleep_for(period * 4);
    }
  });

  std::thread faster_consumer([this, &target]() {
    int i = 0;
    while (i < TOTAL_ITEMS - 1) {
      buffer->read([&i, &target](uint8_t* buffer){
        std::memcpy(&target[i++][0], buffer, ITEM_SIZE);
      });
      std::this_thread::sleep_for(period);
    }

    // Due to the lock-free implementation, that last item would not be read, since
    // the reader can not know if it's still being written to. So we have to reset
    // the write index before reading out the buffer.
    reset_writing();
    buffer->read([&i, &target](uint8_t* buffer){
      std::memcpy(&target[i++][0], buffer, ITEM_SIZE);
    });
  });

  slower_producer.join();
  faster_consumer.join();

  ASSERT_TRUE(buffer->empty());
  ASSERT_FALSE(buffer->full());

  // Blocking read and write should be synchronized even if one thread is faster.
  std::cout << "Slower producer, faster consumer:" << std::endl;
  for (int i = 0; i < TOTAL_ITEMS; ++i) {
    std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
//    EXPECT_EQ(target[i], source[i]);
  }

  //TODO: finish asserts
  const bool finishedImplementing = false;
  ASSERT_TRUE(finishedImplementing);
}

TEST_F(ThreadSafeRingBufferTest, ReadWriteToBufferWithOverwriteThrottling) {

  const int TOTAL_ITEMS = 10; // total items to process
  const std::vector<std::string> source = rand_vector_str(TOTAL_ITEMS, ITEM_SIZE);
  std::vector<std::string> target = known_vector_str(TOTAL_ITEMS, "0000");
  static constexpr std::chrono::milliseconds period(10);

  EXPECT_TRUE(buffer->empty());
  EXPECT_FALSE(buffer->full());

  // First, the producer will write to the buffer faster than the consumer can read.
  std::thread faster_producer([this, &source]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->write_overwrite([i, &source](uint8_t* buffer){
        std::memcpy(buffer, &source[i][0], ITEM_SIZE);
      });
      std::this_thread::sleep_for(period);
    }
  });

  std::thread slower_consumer([this, &target, TOTAL_ITEMS]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->read_timeout([i, &target](uint8_t* buffer){
        std::memcpy(&target[i][0], buffer, ITEM_SIZE);
      }, 1s);
      std::this_thread::sleep_for(period * 4);
    }

    // Due to the lock-free implementation, that last item would not be read, since
    // the reader can not know if it's still being written to. So we have to reset
    // the write index before reading out the buffer.
    reset_writing();
    buffer->read_timeout([TOTAL_ITEMS, &target](uint8_t* buffer){
      std::memcpy(&target[TOTAL_ITEMS - 1][0], buffer, ITEM_SIZE);
    }, 1s);
  });

  faster_producer.join();
  slower_consumer.join();

  ASSERT_TRUE(buffer->empty());
  ASSERT_FALSE(buffer->full());

  // Blocking read and write should be synchronized even if one thread is faster.
  std::cout << "Faster producer, slower consumer:" << std::endl;
  for (int i = 0; i < TOTAL_ITEMS; ++i) {
    std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
//    EXPECT_EQ(target[i], source[i]);
  }

  target = known_vector_str(TOTAL_ITEMS, "0000");

  // Then, then consumer will read faster than the producer can write.
  std::thread slower_producer([this, &source]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->write_overwrite([i, &source](uint8_t* buffer){
        std::memcpy(buffer, &source[i][0], ITEM_SIZE);
      });
      std::this_thread::sleep_for(period * 4);
    }
  });

  std::thread faster_consumer([this, &target]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->read_timeout([i, &target](uint8_t* buffer){
        std::memcpy(&target[i][0], buffer, ITEM_SIZE);
      }, 1s);
      std::this_thread::sleep_for(period);
    }
  });

  slower_producer.join();
  faster_consumer.join();

  // Blocking read and write should be synchronized even if one thread is faster.
  std::cout << "Slower producer, faster consumer:" << std::endl;
  for (int i = 0; i < TOTAL_ITEMS; ++i) {
    std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
//    EXPECT_EQ(target[i], source[i]);
  }

  //TODO: finish asserts
  const bool finishedImplementing = false;
  ASSERT_TRUE(finishedImplementing);
}

TEST_F(ThreadSafeRingBufferTest, ReadWriteToBufferNonblockingThrottling) {

  const int TOTAL_ITEMS = 10; // total items to process
  const std::vector<std::string> source = rand_vector_str(TOTAL_ITEMS, ITEM_SIZE);
  std::vector<std::string> target = known_vector_str(TOTAL_ITEMS, "0000");
  static constexpr std::chrono::milliseconds period(10);

  EXPECT_TRUE(buffer->empty());
  EXPECT_FALSE(buffer->full());

  // First, the producer will write to the buffer faster than the consumer can read.
  std::thread faster_producer([this, &source]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->write_nonblock([i, &source](uint8_t* buffer){
        std::memcpy(buffer, &source[i][0], ITEM_SIZE);
      });
      std::this_thread::sleep_for(period);
    }
  });

  std::thread slower_consumer([this, &target, TOTAL_ITEMS]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->read_nonblock([i, &target](uint8_t* buffer){
        std::memcpy(&target[i][0], buffer, ITEM_SIZE);
      });
      std::this_thread::sleep_for(period * 4);
    }

    reset_writing();
    buffer->read_nonblock([TOTAL_ITEMS, &target](uint8_t* buffer){
      std::memcpy(&target[TOTAL_ITEMS - 1][0], buffer, ITEM_SIZE);
    });
  });

  faster_producer.join();
  slower_consumer.join();

  ASSERT_TRUE(buffer->empty());
  ASSERT_FALSE(buffer->full());

  // Blocking read and write should be synchronized even if one thread is faster.
  std::cout << "Faster producer, slower consumer:" << std::endl;
  for (int i = 0; i < TOTAL_ITEMS; ++i) {
    std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
    //    EXPECT_EQ(target[i], source[i]);
  }

  target = known_vector_str(TOTAL_ITEMS, "0000");

  // Then, then consumer will read faster than the producer can write.
  std::thread slower_producer([this, &source]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->write_nonblock([i, &source](uint8_t* buffer){
        std::memcpy(buffer, &source[i][0], ITEM_SIZE);
      });
      std::this_thread::sleep_for(period * 4);
    }
  });

  std::thread faster_consumer([this, &target]() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
      buffer->read_nonblock([i, &target](uint8_t* buffer){
        std::memcpy(&target[i][0], buffer, ITEM_SIZE);
      });
      std::this_thread::sleep_for(period);
    }
  });

  slower_producer.join();
  faster_consumer.join();

  // Blocking read and write should be synchronized even if one thread is faster.
  std::cout << "Slower producer, faster consumer:" << std::endl;
  for (int i = 0; i < TOTAL_ITEMS; ++i) {
    std::cout << "source " << source[i] << ", target " << target[i] << std::endl;
    //    EXPECT_EQ(target[i], source[i]);
  }

  //TODO: finish asserts
  const bool finishedImplementing = false;
  ASSERT_TRUE(finishedImplementing);
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}