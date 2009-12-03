#include "Util/SynchronousQueue.h"
#include "Python.h"
#include "gtest/gtest.h"
#include "pythread.h"

#include <algorithm>
#include <vector>
#include <time.h>

// This is a simple event class implemented using pythreads.  Due to the
// limitations of the pythreads API, it supports only one listener and one
// notifier.  It will support multiple events, however.
class SingleWaiterEvent {
public:
    SingleWaiterEvent() {
        this->lock_ = PyThread_allocate_lock();
        PyThread_acquire_lock(this->lock_, /*block=*/1);
    }

    ~SingleWaiterEvent() {
        // Unlock the lock so it's safe to free it.  This assumes that no one
        // is using the lock or the event anymore, which should be the case
        // since we're destroying the event.
        PyThread_acquire_lock(this->lock_, /*block=*/0);
        PyThread_release_lock(this->lock_);

        PyThread_free_lock(this->lock_);
    }

    // Notify the waiter.
    void Notify() {
        PyThread_acquire_lock(this->lock_, /*block=*/0);
        PyThread_release_lock(this->lock_);
    }

    // Wait for notification.
    void Wait() {
        PyThread_acquire_lock(this->lock_, /*block=*/1);
    }

private:
    PyThread_type_lock lock_;
};

TEST(SynchronousQueueTest, SingleThreadPushPop)
{
    SynchronousQueue<int> queue;
    queue.push_back(1);
    queue.push_back(2);
    EXPECT_EQ(1, queue.pop_front());
    EXPECT_EQ(2, queue.pop_front());
    queue.push_back(3);
    queue.push_back(4);
    EXPECT_EQ(3, queue.pop_front());
    EXPECT_EQ(4, queue.pop_front());
}

struct ExpectedData {
    SynchronousQueue<int> *queue_;
    std::vector<int> ints_;
    SingleWaiterEvent done_event_;
};

static void
consumer(void *arg)
{
    ExpectedData *data = (ExpectedData*)arg;
    data->ints_.push_back(data->queue_->pop_front());
    data->ints_.push_back(data->queue_->pop_front());
    data->ints_.push_back(data->queue_->pop_front());
    data->ints_.push_back(data->queue_->pop_front());
    data->ints_.push_back(data->queue_->pop_front());
    std::sort(data->ints_.begin(), data->ints_.end());
    data->done_event_.Notify();
}

// This is how many nanoseconds there are in a millisecond.
static const long MILLISECOND = 1000 * 1000;

static void
millisleep(long millis)
{
    struct timespec rqtp;
    rqtp.tv_sec = 0;
    rqtp.tv_nsec = millis * MILLISECOND;
    nanosleep(&rqtp, NULL);
}

TEST(SynchronousQueueTest, OneConsumerOneProducer)
{
    ExpectedData data;
    SynchronousQueue<int> queue;
    data.queue_ = &queue;

    PyThread_start_new_thread(consumer, &data);

    // Wait a bit before feeding the consumer data so the consumer thread has to
    // block while it waits for us.
    millisleep(1);
    queue.push_back(1);
    queue.push_back(2);
    queue.push_back(3);
    queue.push_back(4);
    queue.push_back(5);

    // Wait until it's done.
    data.done_event_.Wait();

    EXPECT_EQ((size_t)5, data.ints_.size());
    EXPECT_EQ(1, data.ints_[0]);
    EXPECT_EQ(2, data.ints_[1]);
    EXPECT_EQ(3, data.ints_[2]);
    EXPECT_EQ(4, data.ints_[3]);
    EXPECT_EQ(5, data.ints_[4]);
}

static void
producer(void *arg)
{
    SynchronousQueue<int> *queue = (SynchronousQueue<int>*)arg;
    queue->push_back(1);
    millisleep(1);  // Wait here to interleave with the other thread.
    queue->push_back(3);
    queue->push_back(5);
}

TEST(SynchronousQueueTest, OneConsumerTwoProducers)
{
    ExpectedData data;
    SynchronousQueue<int> queue;
    data.queue_ = &queue;

    PyThread_start_new_thread(consumer, &data);

    // Wait a bit before feeding the consumer data so the consumer thread has to
    // block while it waits for us.
    millisleep(1);
    PyThread_start_new_thread(producer, &queue);
    queue.push_back(2);
    millisleep(1);  // Wait here to interleave with the other thread.
    queue.push_back(4);

    // Wait until it's done.
    data.done_event_.Wait();

    EXPECT_EQ(5U, data.ints_.size());
    EXPECT_EQ(1, data.ints_[0]);
    EXPECT_EQ(2, data.ints_[1]);
    EXPECT_EQ(3, data.ints_[2]);
    EXPECT_EQ(4, data.ints_[3]);
    EXPECT_EQ(5, data.ints_[4]);
}

static const int MANY_ITEMS = 100000;

static void
consume_many_items(void *arg)
{
    ExpectedData *data = (ExpectedData*)arg;
    for (int i = 0; i < MANY_ITEMS; ++i) {
        data->ints_.push_back(data->queue_->pop_front());
    }
    std::sort(data->ints_.begin(), data->ints_.end());
    data->done_event_.Notify();
}

TEST(SynchronousQueueTest, ManyItems)
{
    ExpectedData data;
    SynchronousQueue<int> queue;
    data.queue_ = &queue;

    PyThread_start_new_thread(consume_many_items, &data);

    for (int i = 0; i < MANY_ITEMS; ++i) {
        queue.push_back(i);
    }

    // Wait until it's done.
    data.done_event_.Wait();

    EXPECT_EQ((size_t)MANY_ITEMS, data.ints_.size());
    for (int i = 0; i < MANY_ITEMS; ++i) {
        EXPECT_EQ(i, data.ints_[i]);
    }
}
