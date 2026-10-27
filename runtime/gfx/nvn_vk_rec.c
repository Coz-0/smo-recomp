/* The recorder thread (SMO_VK_RECORDER=1): the render thread's command recording and submissions made by a thread of
 * their own. Included by nvn_vk_draw.c.
 *
 * The render thread translates the game's command lists and made every vkCmd* call, the descriptor writes and the
 * submissions itself, so the driver's work behind those calls was in series with the translation. With the recorder,
 * each such call writes its arguments by value into a packet of a ring, and this thread makes the real calls in the
 * same order while the render thread goes on with the next draw. The switch is in the function pointers (rec_start):
 * the code that records is the same with the recorder on or off.
 *
 * What keeps it correct:
 *  - A packet holds copies of everything its call points to: the recorder reads nothing else of the render thread's.
 *  - Vulkan objects a packet names are made before it is queued, and destroyed only once the submission that used
 *    them has finished on the GPU, which the recorder submitted after recording them (packets run in order).
 *  - Fences: waits stay on the render thread. A fence whose submission is still in the ring keeps its waiter waiting
 *    until the recorder submits it, as a fence the GPU has not reached yet would. Before the render thread waits for a
 *    fence, reads its status or resets it (and the descriptor pools of its command buffer), the recorder must have
 *    returned from that submission (rec_reach): vkQueueSubmit's fence is used by one thread at a time (the validation
 *    layer's threading check caught a vkWaitForFences beside it).
 *  - Command buffers: begun, ended and reset by packets; the render thread reuses one only after its fence (above).
 *  - Descriptor pools: sets are allocated and pools reset on the render thread (a pool is reset when its command
 *    buffer has finished, after the recorder wrote its sets); the writes are packets. Vulkan synchronizes the pool
 *    only for allocation and reset, and each set only for its own writes.
 *  - The queue and the swapchain: vkQueuePresentKHR and vkDeviceWaitIdle first wait for the recorder to catch up
 *    (rec_drain), so it never uses the queue at the same time as the render thread.
 *
 * Shared state: the ring's bytes are written by the render thread before it publishes `head` (release) and read by the
 * recorder after it loads `head` (acquire); the render thread reuses them only behind `tail` (the recorder's release
 * store after each packet). The sleeping flags and the condition variables are under g_rec_lock. */
#if defined(__x86_64__) || defined(_M_X64)
#define REC_PAUSE() __builtin_ia32_pause()
#else
#define REC_PAUSE() YieldProcessor()
#endif
typedef void (*RecFn)(const void *args);
typedef struct { RecFn fn; uint32_t size, pad; } RecHdr;     /* fn NULL: the rest of the ring is skipped */
#define REC_RING (32u << 20)                       /* a power of two: about ten Metro frames of packets */
#define AL16(n) (((size_t)(n) + 15) & ~(size_t)15)
static uint8_t *g_rec_ring;
static uint64_t g_rec_head, g_rec_tail;            /* (atomic) published end (render thread), recorded end (recorder) */
static uint64_t g_rec_wpos, g_rec_pub;            /* (render thread) the end of what it has written, and of what it has published */
static uint64_t g_rec_batch = 64u << 10;          /* bytes written before they are published (SMO_VK_REC_BATCH, experiment) */
static LONG64 g_rec_spin_us;                       /* the recorder spins this long for more before it sleeps (SMO_VK_REC_SPIN_US, experiment; 0 by default:
                                                    * 50 us with 4 KB batches kept it spinning through the whole replay, 9 ms of CPU a frame in Metro, and the game's
                                                    * main thread lost more than the render thread gained) */
static int g_rec_on;
static int g_rec_asleep, g_rec_waiters;            /* (atomic) the recorder sleeps on g_rec_work; render thread waits on g_rec_done */
static SRWLOCK g_rec_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_rec_work = CONDITION_VARIABLE_INIT, g_rec_done = CONDITION_VARIABLE_INIT;
static LONG64 g_rec_busy_tk, g_rec_wakes;          /* (recorder) */
static LONG64 g_rec_packets, g_rec_bytes, g_rec_drains, g_rec_drain_tk, g_rec_full_tk;      /* (render thread) */

static void rec_publish(void) {
    if (g_rec_pub == g_rec_wpos) return;
    g_rec_pub = g_rec_wpos;
    __atomic_store_n(&g_rec_head, g_rec_wpos, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_rec_asleep, __ATOMIC_SEQ_CST)) { AcquireSRWLockExclusive(&g_rec_lock); WakeConditionVariable(&g_rec_work); ReleaseSRWLockExclusive(&g_rec_lock); }
}
/* the render thread waits until the recorder has recorded up to `pos` */
static uint64_t g_rec_tail_seen;                   /* (render thread) g_rec_tail when last read: most waits are answered without touching its line */
static void rec_wait_tail(uint64_t pos, LONG64 *ticks) {
    if (g_rec_tail_seen >= pos || (g_rec_tail_seen = __atomic_load_n(&g_rec_tail, __ATOMIC_ACQUIRE)) >= pos) return;
    LONG64 t0 = tk();
    rec_publish();
    __atomic_fetch_add(&g_rec_waiters, 1, __ATOMIC_SEQ_CST);
    AcquireSRWLockExclusive(&g_rec_lock);
    while (__atomic_load_n(&g_rec_tail, __ATOMIC_SEQ_CST) < pos) SleepConditionVariableSRW(&g_rec_done, &g_rec_lock, 1, 0);
    ReleaseSRWLockExclusive(&g_rec_lock);
    __atomic_fetch_sub(&g_rec_waiters, 1, __ATOMIC_SEQ_CST);
    g_rec_tail_seen = __atomic_load_n(&g_rec_tail, __ATOMIC_ACQUIRE);
    *ticks += tk() - t0;
}
static void rec_drain(void) { if (g_rec_on) { g_rec_drains++; rec_wait_tail(g_rec_wpos, &g_rec_drain_tk); } }
static uint64_t rec_pos(void) { return g_rec_wpos; }
static void rec_reach(uint64_t pos) { if (g_rec_on) rec_wait_tail(pos, &g_rec_drain_tk); }      /* the recorder has made every call queued before pos */

/* n bytes after wpos are free: the recorder has passed them in the ring's last lap */
static void rec_wait_room(uint64_t n) { if (g_rec_wpos + n > REC_RING) rec_wait_tail(g_rec_wpos + n - REC_RING, &g_rec_full_tk); }
/* room for a packet of `size` bytes of arguments; returns where they go. The packet is the recorder's once published. */
static void *rec_push(RecFn fn, size_t size) {
    uint64_t need = AL16(sizeof(RecHdr) + size);
    if (need > REC_RING / 4) vkd_fail("the recorder thread: a packet of %llu bytes", (unsigned long long)need);
    uint64_t at = g_rec_wpos & (REC_RING - 1);
    if (at + need > REC_RING) {                    /* no room before the ring's end: a skip packet, then the packet at the start */
        rec_wait_room(REC_RING - at);
        RecHdr *s = (RecHdr *)(g_rec_ring + at);
        s->fn = NULL; s->size = (uint32_t)(REC_RING - at);
        g_rec_wpos += REC_RING - at; at = 0;
    }
    rec_wait_room(need);
    RecHdr *h = (RecHdr *)(g_rec_ring + at);
    h->fn = fn; h->size = (uint32_t)need;
    g_rec_wpos += need;
    g_rec_packets++; g_rec_bytes += (LONG64)need;
    return h + 1;
}
/* packets are published 64 KB at a time (and at every submission and wait): publishing each one moved the line of
 * g_rec_head between the two threads' caches tens of thousands of times a frame, and each publish to a sleeping
 * recorder is a wake-up */
static void rec_commit(void) { if (g_rec_wpos - g_rec_pub >= g_rec_batch) rec_publish(); }
/* a copy of n bytes at *x (inside the packet being filled), 16-aligned; NULL for NULL */
static void *rec_copy(uint8_t **x, const void *src, size_t n) {
    if (!src || !n) return NULL;
    void *d = *x; memcpy(d, src, n); *x += AL16(n);
    return d;
}
static void rec_no_next(const void *next, const char *call) { if (next) vkd_fail("the recorder thread: %s with a pNext chain (not copied)", call); }

static DWORD WINAPI rec_main(LPVOID arg) {
    (void)arg;
    os_pin_host_thread();                          /* a busy runtime thread: away from the console cores, as the render thread */
    uint64_t t = 0;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LONG64 spin = fq.QuadPart * g_rec_spin_us / 1000000;
    for (;;) {
        uint64_t h = __atomic_load_n(&g_rec_head, __ATOMIC_ACQUIRE);
        if (h == t && spin) {
            LONG64 until = tk() + spin;
            while ((h = __atomic_load_n(&g_rec_head, __ATOMIC_ACQUIRE)) == t && tk() < until) REC_PAUSE();
        }
        if (h == t) {
            AcquireSRWLockExclusive(&g_rec_lock);
            __atomic_store_n(&g_rec_asleep, 1, __ATOMIC_SEQ_CST);
            while ((h = __atomic_load_n(&g_rec_head, __ATOMIC_SEQ_CST)) == t) SleepConditionVariableSRW(&g_rec_work, &g_rec_lock, INFINITE, 0);
            __atomic_store_n(&g_rec_asleep, 0, __ATOMIC_SEQ_CST);
            ReleaseSRWLockExclusive(&g_rec_lock);
            g_rec_wakes++;
        }
        LONG64 t0 = tk();
        while (t != h) {
            const RecHdr *p = (const RecHdr *)(g_rec_ring + (t & (REC_RING - 1)));
            if (p->fn) p->fn(p + 1);
            t += p->size;
            __atomic_store_n(&g_rec_tail, t, __ATOMIC_RELEASE);
        }
        g_rec_busy_tk += tk() - t0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);  /* the tail store before the waiters load (rec_wait_tail: the other order) */
        if (__atomic_load_n(&g_rec_waiters, __ATOMIC_SEQ_CST)) { AcquireSRWLockExclusive(&g_rec_lock); WakeAllConditionVariable(&g_rec_done); ReleaseSRWLockExclusive(&g_rec_lock); }
    }
    return 0;
}

/* ---------------------------------------------------------------- the calls
 * rr_<name>: the driver's function; rs_<name>: the stand-in the render thread calls, which queues a packet; rx_<name>:
 * the recorder's side, which makes the call. Calls that take only values are made by the RCn macros (n arguments
 * after the command buffer); the ones that point to arrays copy them into the packet, after the arguments. */
#define RREAL(n) static __typeof__(n) rr_##n;
#define RC_BODY(n, ...) static void rx_##n(const void *p) { const ra_##n *a = p; rr_##n(__VA_ARGS__); }
#define RC0(n) typedef struct { VkCommandBuffer c; } ra_##n; RREAL(n) RC_BODY(n, a->c) \
    static VKAPI_ATTR void VKAPI_CALL rs_##n(VkCommandBuffer c) { ra_##n *a = rec_push(rx_##n, sizeof *a); a->c = c; rec_commit(); }
#define RC1(n, T1) typedef struct { VkCommandBuffer c; T1 a1; } ra_##n; RREAL(n) RC_BODY(n, a->c, a->a1) \
    static VKAPI_ATTR void VKAPI_CALL rs_##n(VkCommandBuffer c, T1 a1) { ra_##n *a = rec_push(rx_##n, sizeof *a); a->c = c; a->a1 = a1; rec_commit(); }
#define RC2(n, T1, T2) typedef struct { VkCommandBuffer c; T1 a1; T2 a2; } ra_##n; RREAL(n) RC_BODY(n, a->c, a->a1, a->a2) \
    static VKAPI_ATTR void VKAPI_CALL rs_##n(VkCommandBuffer c, T1 a1, T2 a2) { ra_##n *a = rec_push(rx_##n, sizeof *a); a->c = c; a->a1 = a1; a->a2 = a2; rec_commit(); }
#define RC3(n, T1, T2, T3) typedef struct { VkCommandBuffer c; T1 a1; T2 a2; T3 a3; } ra_##n; RREAL(n) RC_BODY(n, a->c, a->a1, a->a2, a->a3) \
    static VKAPI_ATTR void VKAPI_CALL rs_##n(VkCommandBuffer c, T1 a1, T2 a2, T3 a3) { ra_##n *a = rec_push(rx_##n, sizeof *a); a->c = c; a->a1 = a1; a->a2 = a2; a->a3 = a3; rec_commit(); }
#define RC4(n, T1, T2, T3, T4) typedef struct { VkCommandBuffer c; T1 a1; T2 a2; T3 a3; T4 a4; } ra_##n; RREAL(n) RC_BODY(n, a->c, a->a1, a->a2, a->a3, a->a4) \
    static VKAPI_ATTR void VKAPI_CALL rs_##n(VkCommandBuffer c, T1 a1, T2 a2, T3 a3, T4 a4) { ra_##n *a = rec_push(rx_##n, sizeof *a); a->c = c; a->a1 = a1; a->a2 = a2; a->a3 = a3; a->a4 = a4; rec_commit(); }
#define RC5(n, T1, T2, T3, T4, T5) typedef struct { VkCommandBuffer c; T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; } ra_##n; RREAL(n) RC_BODY(n, a->c, a->a1, a->a2, a->a3, a->a4, a->a5) \
    static VKAPI_ATTR void VKAPI_CALL rs_##n(VkCommandBuffer c, T1 a1, T2 a2, T3 a3, T4 a4, T5 a5) { ra_##n *a = rec_push(rx_##n, sizeof *a); a->c = c; a->a1 = a1; a->a2 = a2; a->a3 = a3; a->a4 = a4; a->a5 = a5; rec_commit(); }

RC0(vkCmdEndRendering)
RC2(vkCmdBindPipeline, VkPipelineBindPoint, VkPipeline)
RC3(vkCmdSetDepthBias, float, float, float)
RC2(vkCmdSetStencilCompareMask, VkStencilFaceFlags, uint32_t)
RC2(vkCmdSetStencilWriteMask, VkStencilFaceFlags, uint32_t)
RC2(vkCmdSetStencilReference, VkStencilFaceFlags, uint32_t)
RC1(vkCmdSetLineWidth, float)
RC1(vkCmdSetCullMode, VkCullModeFlags)
RC1(vkCmdSetFrontFace, VkFrontFace)
RC1(vkCmdSetPrimitiveTopology, VkPrimitiveTopology)
RC1(vkCmdSetDepthTestEnable, VkBool32)
RC1(vkCmdSetDepthWriteEnable, VkBool32)
RC1(vkCmdSetDepthCompareOp, VkCompareOp)
RC1(vkCmdSetStencilTestEnable, VkBool32)
RC5(vkCmdSetStencilOp, VkStencilFaceFlags, VkStencilOp, VkStencilOp, VkStencilOp, VkCompareOp)
RC1(vkCmdSetDepthBiasEnable, VkBool32)
RC1(vkCmdSetPrimitiveRestartEnable, VkBool32)
RC1(vkCmdSetRasterizerDiscardEnable, VkBool32)
RC3(vkCmdBindIndexBuffer, VkBuffer, VkDeviceSize, VkIndexType)
RC4(vkCmdDraw, uint32_t, uint32_t, uint32_t, uint32_t)
RC5(vkCmdDrawIndexed, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
RC3(vkCmdDispatch, uint32_t, uint32_t, uint32_t)
RC3(vkCmdResetQueryPool, VkQueryPool, uint32_t, uint32_t)
RC3(vkCmdWriteTimestamp, VkPipelineStageFlagBits, VkQueryPool, uint32_t)
RC3(vkCmdBeginQuery, VkQueryPool, uint32_t, VkQueryControlFlags)
RC2(vkCmdEndQuery, VkQueryPool, uint32_t)
RC1(vkCmdSetPolygonModeEXT, VkPolygonMode)
RC1(vkCmdSetDepthClampEnableEXT, VkBool32)
RC1(vkCmdSetAlphaToCoverageEnableEXT, VkBool32)

/* command buffers: the result of the real call is checked on the recorder (a failure ends the process there) */
typedef struct { VkCommandBuffer c; VkCommandBufferBeginInfo bi; } ra_vkBeginCommandBuffer; RREAL(vkBeginCommandBuffer)
static void rx_vkBeginCommandBuffer(const void *p) { const ra_vkBeginCommandBuffer *a = p; VKC(rr_vkBeginCommandBuffer(a->c, &a->bi)); }
static VkResult VKAPI_CALL rs_vkBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *bi) {
    rec_no_next(bi->pNext, "vkBeginCommandBuffer");
    if (bi->pInheritanceInfo) vkd_fail("the recorder thread: vkBeginCommandBuffer with inheritance (not copied)");
    ra_vkBeginCommandBuffer *a = rec_push(rx_vkBeginCommandBuffer, sizeof *a); a->c = c; a->bi = *bi; rec_commit();
    return VK_SUCCESS;
}
typedef struct { VkCommandBuffer c; } ra_vkEndCommandBuffer; RREAL(vkEndCommandBuffer)
static void rx_vkEndCommandBuffer(const void *p) { const ra_vkEndCommandBuffer *a = p; VKC(rr_vkEndCommandBuffer(a->c)); }
static VkResult VKAPI_CALL rs_vkEndCommandBuffer(VkCommandBuffer c) { ra_vkEndCommandBuffer *a = rec_push(rx_vkEndCommandBuffer, sizeof *a); a->c = c; rec_commit(); return VK_SUCCESS; }
typedef struct { VkCommandBuffer c; VkFlags f; } ra_vkResetCommandBuffer; RREAL(vkResetCommandBuffer)
static void rx_vkResetCommandBuffer(const void *p) { const ra_vkResetCommandBuffer *a = p; VKC(rr_vkResetCommandBuffer(a->c, a->f)); }
static VkResult VKAPI_CALL rs_vkResetCommandBuffer(VkCommandBuffer c, VkFlags f) { ra_vkResetCommandBuffer *a = rec_push(rx_vkResetCommandBuffer, sizeof *a); a->c = c; a->f = f; rec_commit(); return VK_SUCCESS; }

typedef struct { VkQueue q; uint32_t n; const VkSubmitInfo *si; VkFence f; } ra_vkQueueSubmit; RREAL(vkQueueSubmit)
static void rx_vkQueueSubmit(const void *p) { const ra_vkQueueSubmit *a = p; VKC(rr_vkQueueSubmit(a->q, a->n, a->si, a->f)); }
static VkResult VKAPI_CALL rs_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    size_t extra = AL16(n * sizeof *si);
    for (uint32_t i = 0; i < n; i++) {
        rec_no_next(si[i].pNext, "vkQueueSubmit");
        extra += 2 * AL16(si[i].waitSemaphoreCount * sizeof(VkSemaphore)) + AL16(si[i].commandBufferCount * sizeof(VkCommandBuffer)) + AL16(si[i].signalSemaphoreCount * sizeof(VkSemaphore));
    }
    ra_vkQueueSubmit *a = rec_push(rx_vkQueueSubmit, AL16(sizeof *a) + extra);
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    VkSubmitInfo *d = rec_copy(&x, si, n * sizeof *si);
    for (uint32_t i = 0; i < n; i++) {
        d[i].pWaitSemaphores = rec_copy(&x, si[i].pWaitSemaphores, si[i].waitSemaphoreCount * sizeof(VkSemaphore));
        d[i].pWaitDstStageMask = rec_copy(&x, si[i].pWaitDstStageMask, si[i].waitSemaphoreCount * sizeof(VkPipelineStageFlags));
        d[i].pCommandBuffers = rec_copy(&x, si[i].pCommandBuffers, si[i].commandBufferCount * sizeof(VkCommandBuffer));
        d[i].pSignalSemaphores = rec_copy(&x, si[i].pSignalSemaphores, si[i].signalSemaphoreCount * sizeof(VkSemaphore));
    }
    a->q = q; a->n = n; a->si = d; a->f = f;
    rec_publish();                                 /* the render thread may wait for its fence next */
    return VK_SUCCESS;
}
/* the queue and the swapchain stay the render thread's: the recorder is idle when it uses them (the wait before a
 * present was 0.09 ms a frame in Metro: presents as packets were not worth their rules) */
RREAL(vkQueuePresentKHR)
static VkResult VKAPI_CALL rs_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) { rec_drain(); return rr_vkQueuePresentKHR(q, pi); }
RREAL(vkDeviceWaitIdle)
static VkResult VKAPI_CALL rs_vkDeviceWaitIdle(VkDevice d) { rec_drain(); return rr_vkDeviceWaitIdle(d); }

/* descriptor writes: each write's info array, by its type */
static size_t rec_writes_size(uint32_t n, const VkWriteDescriptorSet *w) {
    size_t s = AL16(n * sizeof *w);
    for (uint32_t i = 0; i < n; i++) {
        rec_no_next(w[i].pNext, "a descriptor write");
        if (w[i].pImageInfo) s += AL16(w[i].descriptorCount * sizeof(VkDescriptorImageInfo));
        if (w[i].pBufferInfo) s += AL16(w[i].descriptorCount * sizeof(VkDescriptorBufferInfo));
        if (w[i].pTexelBufferView) s += AL16(w[i].descriptorCount * sizeof(VkBufferView));
    }
    return s;
}
static VkWriteDescriptorSet *rec_writes(uint8_t **x, uint32_t n, const VkWriteDescriptorSet *w) {
    VkWriteDescriptorSet *d = rec_copy(x, w, n * sizeof *w);
    for (uint32_t i = 0; i < n; i++) {
        switch (w[i].descriptorType) {             /* only the array Vulkan reads for the type: the others may be stale */
        case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            d[i].pImageInfo = rec_copy(x, w[i].pImageInfo, w[i].descriptorCount * sizeof(VkDescriptorImageInfo)); d[i].pBufferInfo = NULL; d[i].pTexelBufferView = NULL; break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            d[i].pTexelBufferView = rec_copy(x, w[i].pTexelBufferView, w[i].descriptorCount * sizeof(VkBufferView)); d[i].pImageInfo = NULL; d[i].pBufferInfo = NULL; break;
        default:
            d[i].pBufferInfo = rec_copy(x, w[i].pBufferInfo, w[i].descriptorCount * sizeof(VkDescriptorBufferInfo)); d[i].pImageInfo = NULL; d[i].pTexelBufferView = NULL; break;
        }
    }
    return d;
}
typedef struct { VkDevice d; uint32_t n; const VkWriteDescriptorSet *w; } ra_vkUpdateDescriptorSets; RREAL(vkUpdateDescriptorSets)
static void rx_vkUpdateDescriptorSets(const void *p) { const ra_vkUpdateDescriptorSets *a = p; rr_vkUpdateDescriptorSets(a->d, a->n, a->w, 0, NULL); }
static void VKAPI_CALL rs_vkUpdateDescriptorSets(VkDevice dev, uint32_t n, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    (void)c;
    if (nc) vkd_fail("the recorder thread: vkUpdateDescriptorSets with descriptor copies (not copied)");
    ra_vkUpdateDescriptorSets *a = rec_push(rx_vkUpdateDescriptorSets, AL16(sizeof *a) + rec_writes_size(n, w));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->d = dev; a->n = n; a->w = rec_writes(&x, n, w);
    rec_commit();
}
typedef struct { VkCommandBuffer c; VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t set, n; const VkWriteDescriptorSet *w; } ra_vkCmdPushDescriptorSetKHR; RREAL(vkCmdPushDescriptorSetKHR)
static void rx_vkCmdPushDescriptorSetKHR(const void *p) { const ra_vkCmdPushDescriptorSetKHR *a = p; rr_vkCmdPushDescriptorSetKHR(a->c, a->bp, a->l, a->set, a->n, a->w); }
static void VKAPI_CALL rs_vkCmdPushDescriptorSetKHR(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n, const VkWriteDescriptorSet *w) {
    ra_vkCmdPushDescriptorSetKHR *a = rec_push(rx_vkCmdPushDescriptorSetKHR, AL16(sizeof *a) + rec_writes_size(n, w));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->bp = bp; a->l = l; a->set = set; a->n = n; a->w = rec_writes(&x, n, w);
    rec_commit();
}

typedef struct { VkCommandBuffer c; VkRenderingInfo ri; } ra_vkCmdBeginRendering; RREAL(vkCmdBeginRendering)
static void rx_vkCmdBeginRendering(const void *p) { const ra_vkCmdBeginRendering *a = p; rr_vkCmdBeginRendering(a->c, &a->ri); }
static void VKAPI_CALL rs_vkCmdBeginRendering(VkCommandBuffer c, const VkRenderingInfo *ri) {
    rec_no_next(ri->pNext, "vkCmdBeginRendering");
    size_t nc = ri->colorAttachmentCount * sizeof(VkRenderingAttachmentInfo), one = sizeof(VkRenderingAttachmentInfo);
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) rec_no_next(ri->pColorAttachments[i].pNext, "a color attachment");
    ra_vkCmdBeginRendering *a = rec_push(rx_vkCmdBeginRendering, AL16(sizeof *a) + AL16(nc) + 2 * AL16(one));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->ri = *ri;
    a->ri.pColorAttachments = rec_copy(&x, ri->pColorAttachments, nc);
    a->ri.pDepthAttachment = rec_copy(&x, ri->pDepthAttachment, one);
    a->ri.pStencilAttachment = rec_copy(&x, ri->pStencilAttachment, one);
    rec_commit();
}
typedef struct { VkCommandBuffer c; VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, nd; const VkDescriptorSet *s; const uint32_t *d; } ra_vkCmdBindDescriptorSets; RREAL(vkCmdBindDescriptorSets)
static void rx_vkCmdBindDescriptorSets(const void *p) { const ra_vkCmdBindDescriptorSets *a = p; rr_vkCmdBindDescriptorSets(a->c, a->bp, a->l, a->first, a->n, a->s, a->nd, a->d); }
static void VKAPI_CALL rs_vkCmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    ra_vkCmdBindDescriptorSets *a = rec_push(rx_vkCmdBindDescriptorSets, AL16(sizeof *a) + AL16(n * sizeof *s) + AL16(nd * sizeof *d));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->bp = bp; a->l = l; a->first = first; a->n = n; a->nd = nd;
    a->s = rec_copy(&x, s, n * sizeof *s); a->d = rec_copy(&x, d, nd * sizeof *d);
    rec_commit();
}
typedef struct { VkCommandBuffer c; VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, size; uint8_t v[128]; } ra_vkCmdPushConstants; RREAL(vkCmdPushConstants)
static void rx_vkCmdPushConstants(const void *p) { const ra_vkCmdPushConstants *a = p; rr_vkCmdPushConstants(a->c, a->l, a->st, a->off, a->size, a->v); }
static void VKAPI_CALL rs_vkCmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    if (size > 128) vkd_fail("the recorder thread: %u bytes of push constants", size);
    ra_vkCmdPushConstants *a = rec_push(rx_vkCmdPushConstants, sizeof *a);
    a->c = c; a->l = l; a->st = st; a->off = off; a->size = size; memcpy(a->v, v, size);
    rec_commit();
}
/* calls of the shape (command buffer, first, count, array) */
#define RARR(n, T) typedef struct { VkCommandBuffer c; uint32_t first, n; const T *v; } ra_##n; RREAL(n) \
    static void rx_##n(const void *p) { const ra_##n *a = p; rr_##n(a->c, a->first, a->n, a->v); } \
    static void VKAPI_CALL rs_##n(VkCommandBuffer c, uint32_t first, uint32_t n, const T *v) { \
        ra_##n *a = rec_push(rx_##n, AL16(sizeof *a) + AL16(n * sizeof *v)); uint8_t *x = (uint8_t *)a + AL16(sizeof *a); \
        a->c = c; a->first = first; a->n = n; a->v = rec_copy(&x, v, n * sizeof *v); rec_commit(); }
RARR(vkCmdSetViewport, VkViewport)
RARR(vkCmdSetScissor, VkRect2D)
RARR(vkCmdSetColorBlendEnableEXT, VkBool32)
RARR(vkCmdSetColorBlendEquationEXT, VkColorBlendEquationEXT)
RARR(vkCmdSetColorWriteMaskEXT, VkColorComponentFlags)
typedef struct { VkCommandBuffer c; float v[4]; } ra_vkCmdSetBlendConstants; RREAL(vkCmdSetBlendConstants)
static void rx_vkCmdSetBlendConstants(const void *p) { const ra_vkCmdSetBlendConstants *a = p; rr_vkCmdSetBlendConstants(a->c, a->v); }
static void VKAPI_CALL rs_vkCmdSetBlendConstants(VkCommandBuffer c, const float v[4]) { ra_vkCmdSetBlendConstants *a = rec_push(rx_vkCmdSetBlendConstants, sizeof *a); a->c = c; memcpy(a->v, v, sizeof a->v); rec_commit(); }
typedef struct { VkCommandBuffer c; VkSampleCountFlagBits n; VkSampleMask m[2]; } ra_vkCmdSetSampleMaskEXT; RREAL(vkCmdSetSampleMaskEXT)
static void rx_vkCmdSetSampleMaskEXT(const void *p) { const ra_vkCmdSetSampleMaskEXT *a = p; rr_vkCmdSetSampleMaskEXT(a->c, a->n, a->m); }
static void VKAPI_CALL rs_vkCmdSetSampleMaskEXT(VkCommandBuffer c, VkSampleCountFlagBits n, const VkSampleMask *m) {
    if (n > 64) vkd_fail("the recorder thread: a sample mask for %u samples", (unsigned)n);
    ra_vkCmdSetSampleMaskEXT *a = rec_push(rx_vkCmdSetSampleMaskEXT, sizeof *a); a->c = c; a->n = n; memcpy(a->m, m, ((size_t)n + 31) / 32 * sizeof *m); rec_commit();
}
typedef struct { VkCommandBuffer c; uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o; } ra_vkCmdBindVertexBuffers; RREAL(vkCmdBindVertexBuffers)
static void rx_vkCmdBindVertexBuffers(const void *p) { const ra_vkCmdBindVertexBuffers *a = p; rr_vkCmdBindVertexBuffers(a->c, a->first, a->n, a->b, a->o); }
static void VKAPI_CALL rs_vkCmdBindVertexBuffers(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    ra_vkCmdBindVertexBuffers *a = rec_push(rx_vkCmdBindVertexBuffers, AL16(sizeof *a) + AL16(n * sizeof *b) + AL16(n * sizeof *o));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->first = first; a->n = n; a->b = rec_copy(&x, b, n * sizeof *b); a->o = rec_copy(&x, o, n * sizeof *o);
    rec_commit();
}
typedef struct { VkCommandBuffer c; uint32_t nb, na; const VkVertexInputBindingDescription2EXT *b; const VkVertexInputAttributeDescription2EXT *at; } ra_vkCmdSetVertexInputEXT; RREAL(vkCmdSetVertexInputEXT)
static void rx_vkCmdSetVertexInputEXT(const void *p) { const ra_vkCmdSetVertexInputEXT *a = p; rr_vkCmdSetVertexInputEXT(a->c, a->nb, a->b, a->na, a->at); }
static void VKAPI_CALL rs_vkCmdSetVertexInputEXT(VkCommandBuffer c, uint32_t nb, const VkVertexInputBindingDescription2EXT *b, uint32_t na, const VkVertexInputAttributeDescription2EXT *at) {
    ra_vkCmdSetVertexInputEXT *a = rec_push(rx_vkCmdSetVertexInputEXT, AL16(sizeof *a) + AL16(nb * sizeof *b) + AL16(na * sizeof *at));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->nb = nb; a->na = na; a->b = rec_copy(&x, b, nb * sizeof *b); a->at = rec_copy(&x, at, na * sizeof *at);
    rec_commit();
}
typedef struct { VkCommandBuffer c; uint32_t n, nr; const VkClearAttachment *at; const VkClearRect *r; } ra_vkCmdClearAttachments; RREAL(vkCmdClearAttachments)
static void rx_vkCmdClearAttachments(const void *p) { const ra_vkCmdClearAttachments *a = p; rr_vkCmdClearAttachments(a->c, a->n, a->at, a->nr, a->r); }
static void VKAPI_CALL rs_vkCmdClearAttachments(VkCommandBuffer c, uint32_t n, const VkClearAttachment *at, uint32_t nr, const VkClearRect *r) {
    ra_vkCmdClearAttachments *a = rec_push(rx_vkCmdClearAttachments, AL16(sizeof *a) + AL16(n * sizeof *at) + AL16(nr * sizeof *r));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->n = n; a->nr = nr; a->at = rec_copy(&x, at, n * sizeof *at); a->r = rec_copy(&x, r, nr * sizeof *r);
    rec_commit();
}
typedef struct { VkCommandBuffer c; VkBuffer b; VkImage i; VkImageLayout l; uint32_t n; const VkBufferImageCopy *r; } ra_vkCmdCopyBufferToImage; RREAL(vkCmdCopyBufferToImage)
static void rx_vkCmdCopyBufferToImage(const void *p) { const ra_vkCmdCopyBufferToImage *a = p; rr_vkCmdCopyBufferToImage(a->c, a->b, a->i, a->l, a->n, a->r); }
static void VKAPI_CALL rs_vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    ra_vkCmdCopyBufferToImage *a = rec_push(rx_vkCmdCopyBufferToImage, AL16(sizeof *a) + AL16(n * sizeof *r));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->b = b; a->i = i; a->l = l; a->n = n; a->r = rec_copy(&x, r, n * sizeof *r);
    rec_commit();
}
typedef struct { VkCommandBuffer c; VkImage i; VkImageLayout l; VkBuffer b; uint32_t n; const VkBufferImageCopy *r; } ra_vkCmdCopyImageToBuffer; RREAL(vkCmdCopyImageToBuffer)
static void rx_vkCmdCopyImageToBuffer(const void *p) { const ra_vkCmdCopyImageToBuffer *a = p; rr_vkCmdCopyImageToBuffer(a->c, a->i, a->l, a->b, a->n, a->r); }
static void VKAPI_CALL rs_vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) {
    ra_vkCmdCopyImageToBuffer *a = rec_push(rx_vkCmdCopyImageToBuffer, AL16(sizeof *a) + AL16(n * sizeof *r));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->i = i; a->l = l; a->b = b; a->n = n; a->r = rec_copy(&x, r, n * sizeof *r);
    rec_commit();
}
typedef struct { VkCommandBuffer c; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const VkImageBlit *r; VkFilter f; } ra_vkCmdBlitImage; RREAL(vkCmdBlitImage)
static void rx_vkCmdBlitImage(const void *p) { const ra_vkCmdBlitImage *a = p; rr_vkCmdBlitImage(a->c, a->s, a->sl, a->d, a->dl, a->n, a->r, a->f); }
static void VKAPI_CALL rs_vkCmdBlitImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    ra_vkCmdBlitImage *a = rec_push(rx_vkCmdBlitImage, AL16(sizeof *a) + AL16(n * sizeof *r));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->s = s; a->sl = sl; a->d = d; a->dl = dl; a->n = n; a->r = rec_copy(&x, r, n * sizeof *r); a->f = f;
    rec_commit();
}
typedef struct { VkCommandBuffer c; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const VkImageResolve *r; } ra_vkCmdResolveImage; RREAL(vkCmdResolveImage)
static void rx_vkCmdResolveImage(const void *p) { const ra_vkCmdResolveImage *a = p; rr_vkCmdResolveImage(a->c, a->s, a->sl, a->d, a->dl, a->n, a->r); }
static void VKAPI_CALL rs_vkCmdResolveImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
    ra_vkCmdResolveImage *a = rec_push(rx_vkCmdResolveImage, AL16(sizeof *a) + AL16(n * sizeof *r));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->s = s; a->sl = sl; a->d = d; a->dl = dl; a->n = n; a->r = rec_copy(&x, r, n * sizeof *r);
    rec_commit();
}
typedef struct { VkCommandBuffer c; VkImage i; VkImageLayout l; VkClearDepthStencilValue v; uint32_t n; const VkImageSubresourceRange *r; } ra_vkCmdClearDepthStencilImage; RREAL(vkCmdClearDepthStencilImage)
static void rx_vkCmdClearDepthStencilImage(const void *p) { const ra_vkCmdClearDepthStencilImage *a = p; rr_vkCmdClearDepthStencilImage(a->c, a->i, a->l, &a->v, a->n, a->r); }
static void VKAPI_CALL rs_vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    ra_vkCmdClearDepthStencilImage *a = rec_push(rx_vkCmdClearDepthStencilImage, AL16(sizeof *a) + AL16(n * sizeof *r));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->i = i; a->l = l; a->v = *v; a->n = n; a->r = rec_copy(&x, r, n * sizeof *r);
    rec_commit();
}
/* these two have nvn_vk.c's own prototypes (VKF), not Vulkan's */
typedef struct { VkCommandBuffer c; VkImage i; VkImageLayout l; VkClearColorValue v; uint32_t n; const VkImageSubresourceRange *r; } ra_vkCmdClearColorImage; RREAL(vkCmdClearColorImage)
static void rx_vkCmdClearColorImage(const void *p) { const ra_vkCmdClearColorImage *a = p; rr_vkCmdClearColorImage(a->c, a->i, a->l, &a->v, a->n, a->r); }
static void VKAPI_PTR rs_vkCmdClearColorImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    ra_vkCmdClearColorImage *a = rec_push(rx_vkCmdClearColorImage, AL16(sizeof *a) + AL16(n * sizeof *r));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->i = i; a->l = l; a->v = *v; a->n = n; a->r = rec_copy(&x, r, n * sizeof *r);
    rec_commit();
}
typedef struct { VkCommandBuffer c; VkFlags src, dst, dep; uint32_t nm, nb, ni; const void *m, *b; const VkImageMemoryBarrier *i; } ra_vkCmdPipelineBarrier; RREAL(vkCmdPipelineBarrier)
static void rx_vkCmdPipelineBarrier(const void *p) { const ra_vkCmdPipelineBarrier *a = p; rr_vkCmdPipelineBarrier(a->c, a->src, a->dst, a->dep, a->nm, a->m, a->nb, a->b, a->ni, a->i); }
static void VKAPI_PTR rs_vkCmdPipelineBarrier(VkCommandBuffer c, VkFlags src, VkFlags dst, VkFlags dep, uint32_t nm, const void *m, uint32_t nb, const void *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    size_t sm = nm * sizeof(VkMemoryBarrier), sb = nb * sizeof(VkBufferMemoryBarrier), si = ni * sizeof *i;
    for (uint32_t k = 0; k < ni; k++) rec_no_next(i[k].pNext, "an image barrier");
    ra_vkCmdPipelineBarrier *a = rec_push(rx_vkCmdPipelineBarrier, AL16(sizeof *a) + AL16(sm) + AL16(sb) + AL16(si));
    uint8_t *x = (uint8_t *)a + AL16(sizeof *a);
    a->c = c; a->src = src; a->dst = dst; a->dep = dep; a->nm = nm; a->nb = nb; a->ni = ni;
    a->m = rec_copy(&x, m, sm); a->b = rec_copy(&x, b, sb); a->i = rec_copy(&x, i, si);
    rec_commit();
}

#define REC_CALLS(X) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdSetDepthBias) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) \
    X(vkCmdSetLineWidth) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) X(vkCmdBindIndexBuffer) \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDispatch) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkQueuePresentKHR) X(vkDeviceWaitIdle) X(vkUpdateDescriptorSets) X(vkCmdBeginRendering) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdBindVertexBuffers) X(vkCmdClearAttachments) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearColorImage) X(vkCmdPipelineBarrier)
/* loaded only with their extensions: swapped when present */
#define REC_CALLS_OPT(X) X(vkCmdPushDescriptorSetKHR) X(vkCmdSetVertexInputEXT) X(vkCmdSetPolygonModeEXT) X(vkCmdSetDepthClampEnableEXT) X(vkCmdSetAlphaToCoverageEnableEXT) \
    X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT) X(vkCmdSetColorWriteMaskEXT) X(vkCmdSetSampleMaskEXT)

/* after vkd_init has loaded every function: from here on the calls above are packets */
static void rec_start(void) {
    const char *e = getenv("SMO_VK_RECORDER");
    if (!e || !atoi(e)) return;
    if (getenv("SMO_VK_REC_SPIN_US")) g_rec_spin_us = atoll(getenv("SMO_VK_REC_SPIN_US"));
    if (getenv("SMO_VK_REC_BATCH")) g_rec_batch = (uint64_t)atoll(getenv("SMO_VK_REC_BATCH"));
    g_rec_ring = VirtualAlloc(NULL, REC_RING, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_rec_ring) vkd_fail("the recorder thread: no memory for its ring");
#define X(n) rr_##n = n; n = rs_##n;
    REC_CALLS(X)
#undef X
#define X(n) if (n) { rr_##n = n; n = rs_##n; }
    REC_CALLS_OPT(X)
#undef X
    HANDLE h = CreateThread(NULL, 0, rec_main, NULL, 0, NULL);
    if (!h) vkd_fail("the recorder thread could not be started");
    CloseHandle(h);
    g_rec_on = 1;
    printf("nvn_vk: SMO_VK_RECORDER=1: Vulkan commands are recorded and submitted by a thread of their own\n");
}
/* for the nvn_vk_perf line, per frame over `frames` frames */
static void rec_report(LONG64 frame, int frames, double k) {
    static LONG64 lb, lw, lp, lby, ld, ldt, lf;
    if (!g_rec_on) return;
    LONG64 busy = g_rec_busy_tk, wakes = g_rec_wakes;
    printf("nvn_vk_rec: frames %lld..%lld per frame: recorder busy %.2f ms, woken %.1f times | %.0f packets, %.0f KB | render thread waited for it: %.2f ms in %.1f drains, %.2f ms for ring room\n",
           (long long)frame - frames, (long long)frame, (double)(busy - lb) * k, (double)(wakes - lw) / frames, (double)(g_rec_packets - lp) / frames, (double)(g_rec_bytes - lby) / 1024.0 / frames,
           (double)(g_rec_drain_tk - ldt) * k, (double)(g_rec_drains - ld) / frames, (double)(g_rec_full_tk - lf) * k);
    lb = busy; lw = wakes; lp = g_rec_packets; lby = g_rec_bytes; ld = g_rec_drains; ldt = g_rec_drain_tk; lf = g_rec_full_tk;
}
