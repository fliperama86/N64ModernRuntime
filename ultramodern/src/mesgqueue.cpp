#include <bitset>
#include <thread>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#ifndef LOD_ENABLE_PIDMA_TRACE
#define LOD_ENABLE_PIDMA_TRACE 0
#endif

#if LOD_ENABLE_PIDMA_TRACE
// [PIDMA] host-level tracing of the generic OS message-queue emulation.
// These hooks are deliberately *not* scoped to a specific mq/source: the
// QueuedMessage staging record (below) does not retain the original
// EventMessageSource past enqueue_external_message_src(), so a PI-DMA
// completion cannot be distinguished from any other message type once it
// reaches do_send()/do_recv(). Logging is restricted to the anomaly paths
// (a failed first-attempt send, a message actually dropped with no retry,
// and a game thread actually blocking on receive) which should be rare
// under healthy operation, keeping this low-noise without needing a
// mq-address filter. See docs/issue27-31-fix-design.md, "Async completion
// loss: lowest-level analysis".
static bool lod_pidma_trace_should_log(uint32_t* counter) {
    uint32_t n = ++(*counter);
    return (n <= 40) || ((n % 500) == 0);
}
static uint32_t lod_pidma_sendfail_calls = 0;
static uint32_t lod_pidma_dropped_calls = 0;
static uint32_t lod_pidma_blocked_calls = 0;
#endif

struct QueuedMessage {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    bool jam;
    bool requeue_if_blocked;
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};
std::bitset<32> requeue_enabled;

void ultramodern::set_message_queue_control(const ultramodern::MessageQueueControl& mqc) {
    requeue_enabled.reset();
    requeue_enabled.set(static_cast<int>(EventMessageSource::Timer), mqc.requeue_timer);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Sp), mqc.requeue_sp);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Si), mqc.requeue_si);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Ai), mqc.requeue_ai);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Vi), mqc.requeue_vi);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Pi), mqc.requeue_pi);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Dp), mqc.requeue_dp);
}

void ultramodern::enqueue_external_message_src(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, EventMessageSource src) {
    if (mq == 0) return;
    // Validate: mq must be a valid N64 KSEK0 address AND the queue must look initialized
    // (msgCount > 0 and <= 1000). Early boot SI messages arrive before queues are created.
    {
        uint32_t mq_val = (uint32_t)mq;
        if (mq_val < 0x80000000 || mq_val > 0x80FFFFFF) return;
    }
    external_messages.enqueue({mq, msg, jam, requeue_enabled[static_cast<int>(src)]});
}

void ultramodern::enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked) {
    if (mq == 0) return;
    // Validate mq
    if ((uint32_t)mq < 0x80000000 || (uint32_t)mq > 0x80FFFFFF) {
        fprintf(stderr, "[EXT_ENQ] INVALID mq=0x%08X — DROPPED\n", (uint32_t)mq);
        return;
    }
    external_messages.enqueue({mq, msg, jam, requeue_if_blocked});
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block);

#if LOD_ENABLE_PIDMA_TRACE
// Called right after a failed do_send() when the caller has just decided
// whether to requeue. If it is NOT going to be requeued, this message is
// gone forever - the exact mechanism the hang under investigation needs.
static void lod_pidma_note_drop_if_permanent(const QueuedMessage& to_send, bool send_ok) {
    if (!send_ok && !to_send.requeue_if_blocked) {
        if (lod_pidma_trace_should_log(&lod_pidma_dropped_calls)) {
            fprintf(stderr, "[PIDMA] dropped mq=0x%08X jam=%d (send failed, not requeued - permanent loss)\n",
                    (uint32_t)to_send.mq, (int)to_send.jam);
        }
    }
}
#endif

void dequeue_external_messages(RDRAM_ARG1) {
    QueuedMessage to_send;
    std::vector<QueuedMessage> requeued_messages{};
    while (external_messages.try_dequeue(to_send)) {
        bool sent = do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
#if LOD_ENABLE_PIDMA_TRACE
        lod_pidma_note_drop_if_permanent(to_send, sent);
#endif
        if (!sent && to_send.requeue_if_blocked) {
            requeued_messages.push_back(to_send);
        }
    }
    for (QueuedMessage& cur_mesg : requeued_messages) {
        external_messages.enqueue(cur_mesg);
    }
}

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
    external_messages.wait_dequeue(to_send);
    bool sent = do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
#if LOD_ENABLE_PIDMA_TRACE
    lod_pidma_note_drop_if_permanent(to_send, sent);
#endif
    if (!sent && to_send.requeue_if_blocked) {
        external_messages.enqueue(to_send);
    }
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG u32 millis) {
    QueuedMessage to_send;
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        bool sent = do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
#if LOD_ENABLE_PIDMA_TRACE
        lod_pidma_note_drop_if_permanent(to_send, sent);
#endif
        if (!sent && to_send.requeue_if_blocked) {
            external_messages.enqueue(to_send);
        }
    }
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    mq->blocked_on_recv = NULLPTR;
    mq->blocked_on_send = NULLPTR;
    mq->msgCount = count;
    mq->msg = msg;
    mq->validCount = 0;
    mq->first = 0;
}

s32 MQ_GET_COUNT(OSMesgQueue *mq) {
    return mq->validCount;
}

s32 MQ_IS_EMPTY(OSMesgQueue *mq) {
    return mq->validCount == 0;
}

s32 MQ_IS_FULL(OSMesgQueue* mq) {
    return MQ_GET_COUNT(mq) >= mq->msgCount;
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    // Sanity check: uninitialized queues have garbage values.
    // Reject messages to queues with impossible counts.
    if (mq->validCount < 0 || mq->validCount > mq->msgCount || mq->msgCount <= 0 || mq->msgCount > 1000) {
#if LOD_ENABLE_PIDMA_TRACE
        // block==false here always means this came from the non-blocking
        // external-message drain (dequeue_external_messages /
        // wait_for_external_message*), i.e. this is a candidate for a
        // silently-lost one-shot completion (PI DMA and any other source
        // whose requeue flag is false for this message).
        if (!block && lod_pidma_trace_should_log(&lod_pidma_sendfail_calls)) {
            fprintf(stderr, "[PIDMA] send-fail mq=0x%08X reason=insane-queue validCount=%d msgCount=%d\n",
                    (uint32_t)mq_, mq->validCount, mq->msgCount);
        }
#endif
        return false;
    }
    if (!block) {
        // If non-blocking, fail if the queue is full.
        if (MQ_IS_FULL(mq)) {
#if LOD_ENABLE_PIDMA_TRACE
            if (lod_pidma_trace_should_log(&lod_pidma_sendfail_calls)) {
                fprintf(stderr, "[PIDMA] send-fail mq=0x%08X reason=full validCount=%d msgCount=%d\n",
                        (uint32_t)mq_, mq->validCount, mq->msgCount);
            }
#endif
            return false;
        }
    }
    else {
        // Otherwise, yield this thread until the queue has room.
        while (MQ_IS_FULL(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on send\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_send), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }
    
    if (jam) {
        // Jams insert at the head of the message queue's buffer.
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[mq->first] = msg;
        mq->validCount++;
    }
    else {
        // Sends insert at the tail of the message queue's buffer.
        s32 last = (mq->first + mq->validCount) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[last] = msg;
        mq->validCount++;
    }

    // If any threads were blocked on receiving from this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }
    
    return true;
}

bool do_recv(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (!block) {
        // If non-blocking, fail if the queue is empty
        if (MQ_IS_EMPTY(mq)) {
            return false;
        }
    } else {
        // Otherwise, yield this thread in a loop until the queue is no longer full
#if LOD_ENABLE_PIDMA_TRACE
        if (MQ_IS_EMPTY(mq) && lod_pidma_trace_should_log(&lod_pidma_blocked_calls)) {
            // The just-preceding dequeue_external_messages() call (done by
            // every osRecvMesg caller before reaching here) did not deliver
            // a message into this queue: the calling game thread is about
            // to genuinely park waiting for one. If this is the async ring's
            // DMA_readWrite call, this is the "one blocking receive per
            // ROM read" wait itself; if no future message ever arrives, this
            // is the observed hang.
            fprintf(stderr, "[PIDMA] blocked mq=0x%08X thread=%d (queue empty, about to wait)\n",
                    (uint32_t)mq_, TO_PTR(OSThread, ultramodern::this_thread())->id);
        }
#endif
        while (MQ_IS_EMPTY(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on receive\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }

    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = TO_PTR(OSMesg, mq->msg)[mq->first];
    }
    
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    // If any threads were blocked on sending to this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_send);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }

    return true;
}

extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = false;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = true;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    
    assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to receive a message.
    bool received = do_recv(PASS_RDRAM mq_, msg_, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return received ? 0 : -1;
}
