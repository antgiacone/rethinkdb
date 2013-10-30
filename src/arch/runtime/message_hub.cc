// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "arch/runtime/message_hub.hpp"

#include <math.h>
#include <unistd.h>

#include "config/args.hpp"
#include "arch/runtime/event_queue.hpp"
#include "arch/runtime/thread_pool.hpp"
#include "logger.hpp"

// Set this to 1 if you would like some "unordered" messages to be unordered.
#ifndef NDEBUG
#define RDB_RELOOP_MESSAGES 0
#endif

linux_message_hub_t::linux_message_hub_t(linux_event_queue_t *queue,
                                         linux_thread_pool_t *thread_pool,
                                         threadnum_t current_thread)
    : queue_(queue),
      thread_pool_(thread_pool),
      is_woken_up_(false),
      current_thread_(current_thread) {

#ifndef NDEBUG
    if(MESSAGE_SCHEDULER_GRANULARITY < (1 << (NUM_SCHEDULER_PRIORITIES))) {
        logWRN("MESSAGE_SCHEDULER_GRANULARITY is too small to honor some of the "
               "lower priorities");
    }
#endif

    queue_->watch_resource(event_.get_notify_fd(), poll_event_in, this);
}

linux_message_hub_t::~linux_message_hub_t() {
    for (int i = 0; i < thread_pool_->n_threads; i++) {
        guarantee(queues_[i].msg_local_list.empty());
    }
    for (int p = MESSAGE_SCHEDULER_MIN_PRIORITY;
         p <= MESSAGE_SCHEDULER_MAX_PRIORITY;
         ++p) {

        guarantee(get_priority_msg_list(p).empty());
    }

    guarantee(incoming_messages_.empty());
}

void linux_message_hub_t::do_store_message(threadnum_t nthread, linux_thread_message_t *msg) {
    rassert(0 <= nthread.threadnum && nthread.threadnum < thread_pool_->n_threads);
    queues_[nthread.threadnum].msg_local_list.push_back(msg);
}

// Collects a message for a given thread onto a local list.
void linux_message_hub_t::store_message_ordered(threadnum_t nthread,
                                                linux_thread_message_t *msg) {
    rassert(!msg->is_ordered); // Each message object can only be enqueued once,
                               // and once it is removed, is_ordered is reset to false.
#ifndef NDEBUG
#if RDB_RELOOP_MESSAGES
    // We default to 1, not zero, to allow store_message_sometime messages to sometimes jump ahead of
    // store_message messages.
    msg->reloop_count_ = 1;
#else
    msg->reloop_count_ = 0;
#endif
#endif  // NDEBUG
    msg->is_ordered = true;
    do_store_message(nthread, msg);
}

int rand_reloop_count() {
    int x;
    frexp(randint(10000) / 10000.0, &x);
    int ret = -x;
    rassert(ret >= 0);
    return ret;
}

void linux_message_hub_t::store_message_sometime(threadnum_t nthread, linux_thread_message_t *msg) {
#ifndef NDEBUG
#if RDB_RELOOP_MESSAGES
    msg->reloop_count_ = rand_reloop_count();
#else
    msg->reloop_count_ = 0;
#endif
#endif  // NDEBUG
    do_store_message(nthread, msg);
}


void linux_message_hub_t::insert_external_message(linux_thread_message_t *msg) {
    bool do_wake_up;
    {
        spinlock_acq_t acq(&incoming_messages_lock_);
        do_wake_up = !check_and_set_is_woken_up();
        incoming_messages_.push_back(msg);
    }

    // Wakey wakey eggs and bakey
    if (do_wake_up) {
        event_.wakey_wakey();
    }
}

linux_message_hub_t::msg_list_t &linux_message_hub_t::get_priority_msg_list(int priority) {
    rassert(priority >= MESSAGE_SCHEDULER_MIN_PRIORITY);
    rassert(priority <= MESSAGE_SCHEDULER_MAX_PRIORITY);
    return priority_msg_lists_[priority - MESSAGE_SCHEDULER_MIN_PRIORITY];
}

void linux_message_hub_t::on_event(int events) {
    if (events != poll_event_in) {
        logERR("Unexpected event mask: %d", events);
    }

    // You must read wakey-wakeys so that the pipe-based implementation doesn't fill
    // up and so that poll-based event triggering doesn't infinite-loop.
    event_.consume_wakey_wakeys();

    // Loop until we have processed at least the initial batch of messages.
    size_t num_initial_msgs_left_to_process[NUM_SCHEDULER_PRIORITIES];
    bool initial_batch_has_been_processed = false;
    bool initial_pass = true;
    do {
        if (!initial_pass) {
            // Other threads will automatically push their messages
            // for us into the incoming queue, even while
            // we are in this loop. However we still have to pick up
            // our local messages, since this->push_messages() is not
            // going to be called while we are still running on_event()
            deliver_local_messages();
        }

        // We guarantee to process all messages that are delivered
        // before the first pass. We do not guarantee to deliver
        // incoming messages delivered during !initial_pass!
        // As a consequence, we must reset is_woken_up_ in the first
        // pass, so later incoming messages wake us up again.
        const bool reset_is_woken_up = initial_pass;
        sort_incoming_messages_by_priority(reset_is_woken_up);

        // We store how many messages we have initially for each priority.
        // Those are the messages that we *definitely* have to process during
        // this call to `on_event()`.
        // We *may* process more messages than this, such that more recent
        // messages with a high-priority can bypass older messages with lower
        // priority.
        if (initial_pass) {
            for (int i = 0; i < NUM_SCHEDULER_PRIORITIES; ++i) {
                num_initial_msgs_left_to_process[i] = priority_msg_lists_[i].size();
            }
            initial_pass = false;
        }

        // Compute how many messages of MESSAGE_SCHEDULER_MAX_PRIORITY we process
        // before we check the incoming queues for new messages.
        // We call this the granularity of the message scheduler, and it is
        // MESSAGE_SCHEDULER_GRANULARITY or smaller.
        size_t total_pending_msgs = 0;
        for (int i = 0; i < NUM_SCHEDULER_PRIORITIES; ++i) {
            total_pending_msgs += priority_msg_lists_[i].size();
        }
        const size_t effective_granularity = std::min(total_pending_msgs,
                                                      MESSAGE_SCHEDULER_GRANULARITY);

        // Process a certain number of messages from each priority
        for (int current_priority = MESSAGE_SCHEDULER_MAX_PRIORITY;
             current_priority >= MESSAGE_SCHEDULER_MIN_PRIORITY; --current_priority) {

            // Compute how many messages of `current_priority` we want to process
            // in this pass.
            // The priority has an exponential effect on how many messages
            // get processed, i.e. if we process 8 messages of priority 1 per pass,
            // we are going to process up to 16 messages of priority 2, 32 of
            // priority 3 and so on.
            // However, we process at least one message of each priority level per
            // pass (in case the granularity is too small).
            int priority_exponent = MESSAGE_SCHEDULER_MAX_PRIORITY - current_priority;
            size_t to_process_from_priority = std::max(1ul, effective_granularity >> priority_exponent);

            for (linux_thread_message_t *m = get_priority_msg_list(current_priority).head();
                 m != NULL && to_process_from_priority > 0;
                 m = get_priority_msg_list(current_priority).head()) {

                get_priority_msg_list(current_priority).remove(m);
                --to_process_from_priority;
                if (num_initial_msgs_left_to_process[current_priority - MESSAGE_SCHEDULER_MIN_PRIORITY] > 0) {
                    // About to process one of the initial messages
                    --num_initial_msgs_left_to_process[current_priority - MESSAGE_SCHEDULER_MIN_PRIORITY];
                }

#ifndef NDEBUG
                if (m->reloop_count_ > 0) {
                    --m->reloop_count_;
                    do_store_message(current_thread_, m);
                    continue;
                }
#endif

                m->on_thread_switch();
            }
        }

        // Check if we have to continue in order to fulfill our guarantee
        // to at least process all of the initial messages.
        initial_batch_has_been_processed = true;
        for (int i = 0; i < NUM_SCHEDULER_PRIORITIES; ++i) {
            if (num_initial_msgs_left_to_process[i] > 0) {
                initial_batch_has_been_processed = false;
                break;
            }
        }
    } while (!initial_batch_has_been_processed);
}

void linux_message_hub_t::sort_incoming_messages_by_priority(bool reset_is_woken_up) {
    // We do this in two steps to release the spinlock faster.
    // append_and_clear is a very cheap operation, while
    // assigning each message to a different priority queue
    // is more expensive.

    // 1. Pull the messages
    msg_list_t new_messages;
    {
        spinlock_acq_t acq(&incoming_messages_lock_);
        new_messages.append_and_clear(&incoming_messages_);
        if (reset_is_woken_up) {
            is_woken_up_ = false;
        }
    }

    // 2. Sort the messages into their respective priority queues
    while (linux_thread_message_t *m = new_messages.head()) {
        new_messages.remove(m);
        int effective_priority = m->priority;
        if (m->is_ordered) {
            // Ordered messages are treated as if they had
            // priority MESSAGE_SCHEDULER_ORDERED_PRIORITY.
            // This ensures that they can never bypass another
            // ordered message.
            effective_priority = MESSAGE_SCHEDULER_ORDERED_PRIORITY;
            m->is_ordered = false;
        }
        get_priority_msg_list(effective_priority).push_back(m);
    }
}

void linux_message_hub_t::deliver_local_messages() {
    const int local_thread = thread_pool_->thread_id;

    if (!queues_[local_thread].msg_local_list.empty()) {
        bool do_wake_up;
        {
            spinlock_acq_t acq(&incoming_messages_lock_);
            incoming_messages_.append_and_clear(&queues_[local_thread].msg_local_list);
            do_wake_up = !check_and_set_is_woken_up();
        }
        if (do_wake_up) {
            // Wake ourselves up for another round.
            // While this might seem risky w.r.t. dead-locks when the event pipe
            // is full, it is actually ok because the is_woken_up() flag guarantees
            // that we only ever write one event onto this.
            event_.wakey_wakey();
        }
    }
}

bool linux_message_hub_t::check_and_set_is_woken_up() {
    const bool was_woken_up = is_woken_up_;
    is_woken_up_ = true;
    return was_woken_up;
}

// Pushes messages collected locally global lists available to all
// threads.
void linux_message_hub_t::push_messages() {
    for (int i = 0; i < thread_pool_->n_threads; i++) {
        // Append the local list for ith thread to that thread's global
        // message list.
        thread_queue_t *queue = &queues_[i];
        if (!queue->msg_local_list.empty()) {
            // Transfer messages to the other core

            bool do_wake_up;
            {
                spinlock_acq_t acq(&thread_pool_->threads[i]->message_hub.incoming_messages_lock_);

                // We only need to do a wake up if we're the first people to do a
                // wake up.
                do_wake_up =
                    !thread_pool_->threads[i]->message_hub.check_and_set_is_woken_up();

                thread_pool_->threads[i]->message_hub.incoming_messages_.append_and_clear(&queue->msg_local_list);
            }

            // Wakey wakey, perhaps eggs and bakey
            if (do_wake_up) {
                thread_pool_->threads[i]->message_hub.event_.wakey_wakey();
            }
        }
    }
}
