/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

/*
 * 本文件保留 RemixDB 原有存储实现，并在 MemTable 刷写、显式提交和关闭路径中接入元数据生命周期回调；原有 WAL、压缩、读写和迭代器逻辑保持不变。
 */

#define _GNU_SOURCE

#include "xdb.h"
#include "ctypes.h"
#include "kv.h"
#include "wh.h"
#include "sst.h"
#include "blkio.h"
#include "causal_meta.h"

// 宏定义区 {{{
#define XDB_COMP_CONC ((4)) // 最大压缩并发线程数
#define XDB_REJECT_SIZE_SHIFT ((4)) // 最多拒绝约 6.25% 的记录
#define WAL_BLKSZ ((PGSZ << 6)) // WAL 块大小为 256 KB
// }}} 宏定义区

// 结构体定义区 {{{
struct mt_pair {
    union {
        void * wmt;
        struct wormhole * wmt_wh;
    };
    union {
        void * imt;
        struct wormhole * imt_wh;
    };
    struct mt_pair * next; // 下一个版本号
};

struct wal {
    u8 * buf; // 通过 wring_acquire() 获取的缓冲区；I/O 操作时可能更换
    u64 bufoff; // 当前已写入字节数，不超过 WAL_BLKSZ；追加数据时更新
    u64 woff; // 按 4 KB（PGSZ）对齐的偏移；I/O 操作时更新
    u64 soff; // 最近一次同步完成的偏移，不超过 woff；同步操作或达到设定 I/O 间隔时更新
    u64 write_user; // 追加写入统计量
    u64 write_nbytes; // 追加写入统计量
    u64 version; // 压缩操作时更新

    int fds[2]; // 初始化后固定不变
    struct wring * wring; // 初始化后固定不变
    u64 maxsz; // 文件最大容量；初始化后固定不变
};

// 映射表
struct xdb {
    // 第一行
    struct mt_pair * volatile mt_view;
    u64 padding1[7];

    u64 mtsz; // MemTable 当前大小；由写线程频繁更新
    struct wal wal; // 由写线程频繁更新
    // 非活跃访问字段
    void * mt1;
    void * mt2;
    u32 nr_workers;
    u32 co_per_worker;
    char * worker_cores;
    pthread_t comp_pid;

    // 只读字段
    u64 max_mtsz;
    u64 max_rejsz;
    struct qsbr * qsbr;
    struct msstz * z;
    struct mt_pair mt_views[4];
    int logfd;
    volatile bool running;
    bool tags; // 是否启用标签
    bool causal_meta_current_wmt_processed; // 记录当前可写 MemTable 是否已提交给元数据模块，避免数据库关闭前重复扫描。
    bool padding2[1];

    u64 padding3[7];
    spinlock lock;
};

struct xdb_ref {
    struct xdb * xdb;
    struct msstv * v;
    struct msstv_ref * vref;

    union {
        void * imt_ref;
        struct wormhole * imt_ref_raw;
    };
    union {
        void * wmt_ref;
        struct wormref * wmt_ref_wh;
    };
    union {
        struct mt_pair * mt_view;
        struct qsbr_ref qref;
    };
};

struct xdb_iter {
    struct xdb_ref * db_ref;
    struct mt_pair * mt_view; // 创建迭代器时使用的版本
    struct miter * miter;
    struct coq * coq_parked; // 暂存的 coq 状态
};
// }}} 结构体定义区

// 其他状态区 {{{
static const struct kvmap_api * wmt_api = &kvmap_api_wormhole;
static const struct kvmap_api * imt_api = &kvmap_api_whunsafe;

static inline void
xdb_lock(struct xdb * const xdb)
{
    spinlock_lock(&xdb->lock);
}

static inline void
xdb_unlock(struct xdb * const xdb)
{
    spinlock_unlock(&xdb->lock);
}

static inline bool
xdb_mt_wal_full(struct xdb * const xdb)
{
    // MemTable 或 WAL 已达到容量上限
    // 该条件成立时，写线程需要等待，并应启动压缩
    return (xdb->mtsz >= xdb->max_mtsz) || (xdb->wal.woff >= xdb->wal.maxsz);
}
// }}} 其他状态区

// WAL 管理区 {{{
// 调用时必须持有锁，详见下文
static void
wal_flush(struct wal * const wal)
{
    if (wal->bufoff == 0)
        return;

    const size_t wsize = bits_round_up(wal->bufoff, 12); // 按完整页面对齐
    debug_assert(wsize <= WAL_BLKSZ);
    memset(wal->buf + wal->bufoff, 0, wsize - wal->bufoff);
    wring_write_partial(wal->wring, (off_t)wal->woff, wal->buf, 0, (u32)wsize);
    wal->buf = wring_acquire(wal->wring);
    debug_assert(wal->buf);
    wal->bufoff = 0;
    wal->woff += wsize;
    wal->write_nbytes += wsize;

#define XDB_SYNC_SIZE ((1lu<<26)) // 64 MB
    if ((wal->woff - wal->soff) >= XDB_SYNC_SIZE) {
        // 将 fsync 请求加入队列，但不等待其完成（调用方无需同步等待）
        wring_fsync(wal->wring);
        wal->soff = wal->woff;
    }
}

// 调用时必须持有锁
static void
wal_flush_sync(struct wal * const wal)
{
    wal_flush(wal);

    if (wal->woff != wal->soff) {
        wring_fsync(wal->wring);
        wal->soff = wal->woff;
    }
}

static void
wal_io_complete(struct wal * const wal)
{
    wring_flush(wal->wring);
}

static void
wal_flush_sync_wait(struct wal * const wal)
{
    wal_flush_sync(wal);
    // 等待操作完成
    wal_io_complete(wal);
}

// 调用时必须持有 xdb->lock
static void
wal_append(struct wal * const wal, const struct kv * const kv)
{
    debug_assert(kv);
    // 键值数据及 CRC32C 校验和
    const size_t estsz = sst_kv_vi128_estimate(kv) + sizeof(u32);
    if ((estsz + wal->bufoff) > WAL_BLKSZ)
        wal_flush(wal);

    debug_assert(wal->buf);
    // 写入键值数据
    u8 * const ptr = sst_kv_vi128_encode(wal->buf + wal->bufoff, kv);
    // 在值数据之后写入键的 CRC 校验值
    *(u32 *)ptr = kv->hashlo;
    wal->bufoff += estsz;
    debug_assert(wal->bufoff <= WAL_BLKSZ);
}

static bool
wal_open(struct wal * const wal, const char * const path)
{
    char * const fn = malloc(strlen(path) + 10);
    if (!fn)
        return false;

    sprintf(fn, "%s/wal1", path);
    const int fd1 = open(fn, O_RDWR|O_CREAT, 00644);
    if (fd1 < 0) {
        fprintf(stderr, "%s open %s failed\n", __func__, fn);
        goto fail_open1;
    }
    wal->fds[0] = fd1;

    sprintf(fn, "%s/wal2", path);
    const int fd2 = open(fn, O_RDWR|O_CREAT, 00644);
    if (fd2 < 0) {
        fprintf(stderr, "%s open %s failed\n", __func__, fn);
        goto fail_open2;
    }
    wal->fds[1] = fd2;

    // 恢复过程中可能替换文件描述符
    wal->wring = wring_create(fd1, WAL_BLKSZ, 32);
    if (!wal->wring)
        goto fail_wring;

    wal->buf = wring_acquire(wal->wring);
    if (!wal->buf)
        goto fail_buf;

    free(fn);
    return true;

    fail_buf:
    wring_destroy(wal->wring);
    wal->wring = NULL;
    fail_wring:
    close(fd2);
    fail_open2:
    close(fd1);
    fail_open1:
    free(fn);
    return false;
}

// 调用时必须持有锁
// 返回旧 WAL 的大小
static u64
wal_switch(struct wal * const wal, const u64 version)
{
    wal_flush_sync_wait(wal);
    const u64 woff0 = wal->woff;
    // bufoff 已经重置为 0
    wal->woff = 0;
    wal->soff = 0;

    // 交换文件描述符
    const int fd1 = wal->fds[0];
    wal->fds[0] = wal->fds[1];
    wal->fds[1] = fd1;
    wring_update_fd(wal->wring, wal->fds[0]);

    memcpy(wal->buf, &version, sizeof(version));
    wal->bufoff = sizeof(version);
    wal->version = version;

    return woff0;
}

static void
wal_close(struct wal * const wal)
{
    wal_flush_sync_wait(wal);
    wring_destroy(wal->wring); // 销毁时会调用 wring_flush()

    close(wal->fds[0]);
    close(wal->fds[1]);
}
// }}} WAL 管理区

// KV 内存分配区 {{{
// 额外分配 1 字节用于引用计数
static struct kv *
xdb_new_ts(const struct kref * const kref)
{
    const size_t sz = sizeof(struct kv) + kref->len; // 不包含 value
    struct kv * const new = malloc(sz);
    debug_assert(new);
    new->klen = kref->len;
    new->vlen = SST_VLEN_TS;
    memcpy(new->kv, kref->ptr, kref->len);
    new->hash = kv_crc32c_extend(kref->hash32); // 此处为何需要固定该值？
    return new;
}

static struct kv *
xdb_dup_kv(const struct kv * const kv)
{
    const size_t sz = sst_kv_size(kv);
    struct kv * const new = malloc(sz);
    debug_assert(new);
    memcpy(new, kv, sz);
    return new;
}
// }}} KV 内存分配区

// xdb_ref 生命周期管理区 {{{
static inline void
xdb_ref_enter(struct xdb_ref * const ref)
{
    if (ref->wmt_ref)
        wmt_api->resume(ref->wmt_ref);
}

static inline void
xdb_ref_leave(struct xdb_ref * const ref)
{
    if (ref->wmt_ref)
        wmt_api->park(ref->wmt_ref);
}

static void
xdb_unref_all(struct xdb_ref * const ref)
{
    if (ref->v) {
        msstv_unref(ref->vref);
        msstz_putv(ref->xdb->z, ref->v);
        ref->v = NULL;
        ref->vref = NULL;
    }

    if (ref->imt_ref) {
        kvmap_unref(imt_api, ref->imt_ref);
        ref->imt_ref = NULL;
    }

    if (ref->wmt_ref) {
        kvmap_unref(wmt_api, ref->wmt_ref);
        ref->wmt_ref = NULL;
    }
    cpu_cfence();
    ref->mt_view = NULL;
    // 无需清零内存
}

// 调用该函数前必须已经释放全部相关资源
static void
xdb_ref_all(struct xdb_ref * const ref)
{
    ref->mt_view = ref->xdb->mt_view;
    ref->v = msstz_getv(ref->xdb->z);
    ref->vref = msstv_ref(ref->v);

    ref->wmt_ref = kvmap_ref(wmt_api, ref->mt_view->wmt);
    debug_assert(ref->wmt_ref);

    if (ref->mt_view->imt) {
        ref->imt_ref = kvmap_ref(imt_api, ref->mt_view->imt);
        debug_assert(ref->imt_ref);
    }
    xdb_ref_leave(ref);
}

static inline void
xdb_ref_update_version(struct xdb_ref * const ref)
{
    if (unlikely(ref->xdb->mt_view != ref->mt_view)) {
        xdb_unref_all(ref);
        xdb_ref_all(ref);
    }
}

struct xdb_ref *
xdb_ref(struct xdb * const xdb)
{
    struct xdb_ref * ref = calloc(1, sizeof(*ref));
    ref->xdb = xdb;
    qsbr_register(xdb->qsbr, &ref->qref);
    xdb_ref_all(ref);
    return ref;
}

struct xdb *
xdb_unref(struct xdb_ref * const ref)
{
    struct xdb * xdb = ref->xdb;
    xdb_unref_all(ref);
    qsbr_unregister(xdb->qsbr, &ref->qref);
    free(ref);
    return xdb;
}
// }}} xdb_ref 生命周期管理区

// 重新插入逻辑区 {{{
struct xdb_reinsert_merge_ctx {
    struct kv * kv;
    struct xdb * xdb;
};

static struct kv *
xdb_mt_reinsert_func(struct kv * const kv0, void * const priv)
{
    struct xdb_reinsert_merge_ctx * const ctx = priv;
    if (kv0 == NULL) {
        struct kv * const ret = xdb_dup_kv(ctx->kv);
        debug_assert(ret);
        const size_t incsz = sst_kv_size(ret);
        struct xdb * const xdb = ctx->xdb;
        xdb_lock(xdb);
        xdb->mtsz += incsz;
        wal_append(&xdb->wal, ret);
        xdb_unlock(xdb);
        return ret;
    } else { // 不覆盖已有记录
        return kv0;
    }
}

// 将 IMT 中被拒绝的键重新插入 WMT；vlen == 1 表示该分区被拒绝
static void
xdb_reinsert_rejected(struct xdb * const xdb, void * const wmt_map, void * const imt_map, struct kv ** const anchors)
{
    void * const wmt_ref = kvmap_ref(wmt_api, wmt_map);
    void * const rej_ref = kvmap_ref(imt_api, imt_map);
    void * const rej_iter = imt_api->iter_create(rej_ref);
    struct xdb_reinsert_merge_ctx ctx = {.xdb = xdb}; // 仅使用 newkv 和 success 字段

    for (u32 i = 0; anchors[i]; i++) {
        if (anchors[i]->vlen == 0) // 跳过已接受的分区
            continue;
        // 定位当前分区的末尾
        if (anchors[i+1]) {
            struct kv * const kz = anchors[i+1];
            struct kref krefz;
            kref_ref_kv_hash32(&krefz, kz);
            imt_api->iter_seek(rej_iter, &krefz);
        }
        // peek 和 next 均不复制数据；参见 mm_mt.out
        struct kv * const end = anchors[i+1] ? imt_api->iter_peek(rej_iter, NULL) : NULL;
        struct kv * const k0 = anchors[i];
        struct kref kref0;
        kref_ref_kv_hash32(&kref0, k0);
        imt_api->iter_seek(rej_iter, &kref0);
        while (imt_api->iter_valid(rej_iter)) {
            struct kv * const curr = imt_api->iter_next(rej_iter, NULL); // 不进行数据复制
            if (curr == end)
                break;

            if (!curr)
                debug_die();
            ctx.kv = curr;
            bool s = kvmap_kv_merge(wmt_api, wmt_ref, curr, xdb_mt_reinsert_func, &ctx);
            if (!s)
                debug_die();
        }
    }
    imt_api->iter_destroy(rej_iter);
    kvmap_unref(imt_api, rej_ref);
    kvmap_unref(wmt_api, wmt_ref);
}
// }}} 重新插入逻辑区


static void
xdb_causal_meta_scan_map(const struct kvmap_api * const api, void * const map)
{
    // 将一个 MemTable 或 Immutable MemTable 作为逻辑页面提交给元数据模块；该函数只扫描内存表，不读取磁盘页面。
    if (!causal_meta_xdb_enabled() || !api || !map)
        return;

    void * const ref = kvmap_ref(api, map);
    if (!ref)
        return;
    void * const iter = api->iter_create(ref);
    if (!iter) {
        kvmap_unref(api, ref);
        return;
    }

    struct kref start;
    kref_ref_hash32(&start, NULL, 0);
    api->iter_seek(iter, &start);

    causal_meta_xdb_begin_page();
    while (api->iter_valid(iter)) {
        struct kv * const kv = api->iter_peek(iter, NULL);
        if (kv && kv->vlen != SST_VLEN_TS) {
            causal_meta_xdb_add_kv(kv_kptr_c(kv), kv->klen, kv_vptr_c(kv), kv->vlen & SST_VLEN_MASK);
        }
        api->iter_skip1(iter);
    }
    causal_meta_xdb_end_page();

    api->iter_destroy(iter);
    kvmap_unref(api, ref);
}

static void
xdb_causal_meta_flush_current_wmt(struct xdb * const xdb)
{
    // 数据库关闭或实验显式执行 flush 前，可写 MemTable 可能尚未达到刷盘阈值；此处将其作为最后一个逻辑页面提交处理。
    if (!causal_meta_xdb_enabled() || !xdb)
        return;

    xdb_lock(xdb);
    const bool need_scan = !xdb->causal_meta_current_wmt_processed && xdb->mtsz > 0;
    void * const wmt_map = xdb->mt_view ? xdb->mt_view->wmt : NULL;
    if (need_scan)
        xdb->causal_meta_current_wmt_processed = true;
    xdb_unlock(xdb);

    if (need_scan && wmt_map)
        xdb_causal_meta_scan_map(wmt_api, wmt_map);
}


// 压缩流程区 {{{
// 压缩流程：
//   -** 获取 xdb 锁
//       - 将 MemTable 模式从仅 WMT 切换为 WMT+IMT（该步骤应快速完成）
//       - 同步刷新并切换日志
//   -** 释放 xdb 锁
//   - 调用 qsbr_wait，等待用户退出当前 IMT
//   - 保留旧版本，直到新版本可以安全访问
//   - 调用 msstz_comp 执行压缩
//   - 释放 WAL 中已不再需要的数据
//   - 对每个被拒绝的键，若其仍为最新版本，则重新插入 WMT 并追加到新 WAL
//       -** 每处理一个仍有效的被拒绝键时，分别获取并释放 xdb 锁
//   -** 获取 xdb 锁
//       - 刷新新 WAL，并异步提交 fsync（不阻塞等待）
//   -** 释放 xdb 锁
//   - 释放 anchors 数组及旧版本
//   - 切回正常模式（仅 WMT），因为 IMT 中的键此时已位于 WMT 或磁盘分区中
//   - 调用 qsbr_wait，等待用户退出 IMT
//   - 清理 IMT（下次压缩时将其用作新的 WMT）；TODO：该步骤开销较大
//   -** 获取 xdb 锁
//       - 等待 fsync 完成，确保被拒绝的键已安全写入新 WAL
//   -** 释放 xdb 锁
//   - 截断旧 WAL；其中的数据已安全存入 z 或新 WAL
//   - 压缩流程结束
static void
xdb_do_comp(struct xdb * const xdb, const u64 max_rejsz)
{
    const double t0 = time_sec();
    xdb_lock(xdb);

    // 切换 mt_view
    struct mt_pair * const v_comp = xdb->mt_view->next;
    xdb->mt_view = v_comp; // WMT 切换为 WMT+IMT

    // 切换日志文件
    const u64 walsz0 = wal_switch(&xdb->wal, msstz_version(xdb->z) + 1); // 与下一个版本号保持一致
    const u64 mtsz0 = xdb->mtsz;
    xdb->mtsz = 0; // 持锁状态下将 mtsz 重置为 0
    xdb->causal_meta_current_wmt_processed = true; // 切换后新的可写 MemTable 为空，因此暂时无需在关闭前扫描。

    xdb_unlock(xdb);

    void * const wmt_map = v_comp->wmt;
    void * const imt_map = v_comp->imt;
    // 已释放锁
    qsbr_wait(xdb->qsbr, (u64)v_comp);

    // 此时 imt_map 已成为不可变 MemTable，但仍驻留在内存中；在压缩落盘前将其作为一个页面提交给元数据模块。
    xdb_causal_meta_scan_map(imt_api, imt_map);

    struct msstv * const oldv = msstz_getv(xdb->z); // 保持 oldv 存活
    const double t_prep = time_sec();

    // 执行压缩
    msstz_comp(xdb->z, imt_api, imt_map, xdb->nr_workers, xdb->co_per_worker, max_rejsz);
    const double t_comp = time_sec();

    struct kv ** const anchors = msstv_anchors(oldv);
    xdb_reinsert_rejected(xdb, wmt_map, imt_map, anchors);
    const double t_reinsert = time_sec();

    // 刷新并同步；随后将截断旧 WAL
    xdb_lock(xdb);
    wal_flush_sync(&xdb->wal);
    xdb_unlock(xdb);

    free(anchors);
    msstz_putv(xdb->z, oldv);

    struct mt_pair * const v_normal = v_comp->next;
    xdb->mt_view = v_normal;
    qsbr_wait(xdb->qsbr, (u64)v_normal);
    const double t_wait2 = time_sec();

    // qsbr_wait 返回之后
    imt_api->clean(imt_map);
    const double t_clean = time_sec();

    xdb_lock(xdb);
    wal_io_complete(&xdb->wal); // 等待同步操作完成
    xdb_unlock(xdb);

    // I/O 完成后截断旧 WAL
    logger_printf(xdb->logfd, "%s discard wal fd %d sz0 %lu\n", __func__, xdb->wal.fds[1], walsz0);
    ftruncate(xdb->wal.fds[1], 0);
    fdatasync(xdb->wal.fds[1]);
    const double t_sync = time_sec();

    // I/O 统计
    const size_t usr_write = xdb->wal.write_user;
    const size_t wal_write = xdb->wal.write_nbytes;
    const size_t sst_write = msstz_stat_writes(xdb->z);
    const size_t sst_read = msstz_stat_reads(xdb->z); // 读取 I/O（远大于实际磁盘 I/O）；TODO：统计 ckeys 的读取 I/O
    // 写放大（WA）与读放大（RA）
    const double sys_wa = (double)(wal_write + sst_write) / (double)usr_write;
    const double comp_ra = (double)sst_read / (double)usr_write;

    const u64 mb = 1lu<<20;
    logger_printf(xdb->logfd, "%s mtsz %lu walsz %lu write-mb usr %lu wal %lu sst %lu WA %.4lf comp-read-mb %lu RA %.4lf\n",
                  __func__, mtsz0, walsz0, usr_write/mb, wal_write/mb, sst_write/mb, sys_wa, sst_read/mb, comp_ra);
    logger_printf(xdb->logfd, "%s times-ms total %.3lf prep %.3lf comp %.3lf reinsert %.3lf wait2 %.3lf clean %.3lf sync %.3lf\n",
                  __func__, t_clean-t0, t_prep-t0, t_comp-t_prep, t_reinsert-t_comp, t_wait2-t_reinsert, t_clean-t_wait2, t_sync-t_clean);
}

static void
xdb_compaction_worker_pin(struct xdb * const xdb)
{
    if (!strcmp(xdb->worker_cores, "auto")) { // 自动检测
        u32 cores[64];
        const u32 ncores = process_getaffinity_list(64, cores);
        if (ncores < xdb->nr_workers)
            logger_printf(xdb->logfd, "%s WARNING: too few cores: %u cores < %u workers\n", __func__, ncores, xdb->nr_workers);

        const u32 nr = (ncores < XDB_COMP_CONC) ? ncores : XDB_COMP_CONC;
        if (nr == 0) { // 该分支是否确实可能发生？
            logger_printf(xdb->logfd, "%s no cores\n", __func__);
        } else if (nr < ncores) { // 需要更新 CPU 亲和性列表
            u32 cpus[XDB_COMP_CONC];
            for (u32 i = 0; i < nr; i++)
                cpus[i] = cores[ncores - nr + i];
            thread_setaffinity_list(nr, cpus);
            logger_printf(xdb->logfd, "%s cpus %u first %u (auto)\n", __func__, nr, cpus[0]);
        } else {
            logger_printf(xdb->logfd, "%s inherited\n", __func__);
        }
    } else if (strcmp(xdb->worker_cores, "dont")) { // 配置值不是 "dont"
        char ** const tokens = strtoks(xdb->worker_cores, ",");
        u32 nr = 0;
        u32 cpus[XDB_COMP_CONC];
        while ((nr < XDB_COMP_CONC) && tokens[nr]) {
            cpus[nr] = a2u32(tokens[nr]);
            nr++;
        }
        free(tokens);
        thread_setaffinity_list(nr, cpus);
        logger_printf(xdb->logfd, "%s pinning cpus %u arg %s\n", __func__, nr, xdb->worker_cores);
    } else {
        logger_printf(xdb->logfd, "%s unpinned (dont)\n", __func__);
    }
    thread_set_name(pthread_self(), "xdb_comp");
}

static void *
xdb_compaction_worker(void * const ptr)
{
    struct xdb * const xdb = (typeof(xdb))ptr;
    xdb_compaction_worker_pin(xdb);

    while (true) {
        // 线程运行中且当前无需压缩
        const u64 t0 = time_nsec();
        // 等待 MemTable 或日志文件达到容量上限
        while (xdb->running && !xdb_mt_wal_full(xdb))
            usleep(10); // 休眠 1 ms

        if (!xdb->running)
            break;

        const u64 dt = time_diff_nsec(t0);
        logger_printf(xdb->logfd, "%s compaction worker wait-ms %lu\n", __func__, dt / 1000000);
        xdb_do_comp(xdb, xdb->max_rejsz);
    }

    pthread_exit(NULL);
}
// }}} 压缩流程区

// 恢复流程区 {{{
struct wal_kv {
    struct kref kref;
    u32 vlen;
    u32 kvlen;
};
// 记录格式：[klen-vi128、vlen-vi128、key-data、value-data、crc32c]
// 解码成功时返回数据之后的位置，否则返回 NULL
static const u8 *
wal_vi128_decode(const u8 * ptr, const u8 * const end, struct wal_kv * const wal_kv)
{
    const u32 safelen = (u32)((end - ptr) < 10 ? (end - ptr) : 10);
    u32 count = 0;
    for (u32 i = 0; i < safelen; i++) {
        if ((ptr[i] & 0x80) == 0)
            count++;
    }
    // 读取并解码 klen 和 vlen
    if (count < 2)
        return NULL;

    u32 klen, vlen;
    ptr = vi128_decode_u32(ptr, &klen);
    ptr = vi128_decode_u32(ptr, &vlen);
    const u32 kvlen = klen + (vlen & SST_VLEN_MASK);

    // 数据长度
    if ((ptr + kvlen + sizeof(u32)) > end)
        return NULL;

    // 校验和
    const u32 sum1 = kv_crc32c(ptr, klen);
    const u32 sum2 = *(const u32 *)(ptr + kvlen);
    if (sum1 != sum2)
        return NULL;

    wal_kv->kref.len = klen;
    wal_kv->kref.hash32 = sum2;
    wal_kv->kref.ptr = ptr;
    wal_kv->vlen = vlen;
    wal_kv->kvlen = kvlen;
    return ptr + kvlen + sizeof(u32);
}

struct xdb_recover_merge_ctx {
    struct kv * newkv;
    u64 mtsz;
};

// kv_merge_func 回调
// 调用时持有锁
static struct kv *
xdb_recover_update_func(struct kv * const kv0, void * const priv)
{
    struct xdb_recover_merge_ctx * const ctx = priv;
    const size_t newsz = sst_kv_size(ctx->newkv);
    const size_t oldsz = kv0 ? sst_kv_size(kv0) : 0;
    const size_t diffsz = newsz - oldsz;
    debug_assert(ctx->mtsz >= oldsz);
    ctx->mtsz += diffsz;
    return ctx->newkv;
}

// 使用 xdb->mt1、xdb->mtsz 和 xdb->z 记录日志
static u64
xdb_recover_fd(struct xdb * const xdb, const int fd)
{
    const u64 fsize = fdsize(fd);
    if (!fsize)
        return 0;

    u8 * const mem = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mem == MAP_FAILED)
        return 0; // 中止恢复

    void * const wmt_ref = wmt_api->ref(xdb->mt1);
    const u8 * iter = mem + sizeof(u64); // 跳过版本字段
    const u8 * const end = mem + fsize;
    u64 nkeys = 0;
    struct xdb_recover_merge_ctx ctx = {.mtsz = xdb->mtsz};

    while ((iter < end) && ((*iter) == 0))
        iter++;
    while (iter < end) {
        struct wal_kv wal_kv;
        const u8 * const iter1 = wal_vi128_decode(iter, end, &wal_kv);

        // 停止
        if (!iter1)
            break;

        // 插入
        struct kv * const kv = malloc(sizeof(struct kv) + wal_kv.kvlen);
        debug_assert(kv);
        kv->klen = wal_kv.kref.len;
        kv->vlen = wal_kv.vlen;
        kv->hash = kv_crc32c_extend(wal_kv.kref.hash32);
        memcpy(kv->kv, wal_kv.kref.ptr, wal_kv.kvlen);
        ctx.newkv = kv;
        bool s = wmt_api->merge(wmt_ref, &wal_kv.kref, xdb_recover_update_func, &ctx);
        if (!s)
            debug_die();

        iter = iter1;
        nkeys++;
        // 跳过填充用的零字节
        while ((iter < end) && ((*iter) == 0))
            iter++;
    }

    xdb->mtsz = ctx.mtsz;
    wmt_api->unref(wmt_ref);
    munmap(mem, fsize);
    const u64 rsize = (u64)(iter - mem);
    logger_printf(xdb->logfd, "%s fd %d fsize %lu rsize %lu nkeys %lu\n", __func__, fd, fsize, rsize, nkeys);
    return rsize;
}

// xdb 初始化时必须将 WAL 相关状态全部清零
static void
xdb_wal_recover(struct xdb * const xdb)
{
    struct wal * const wal = &xdb->wal;
    u64 vs[2] = {};
    for (u32 i = 0; i < 2; i++) {
        if (fdsize(wal->fds[i]) > sizeof(u64))
            pread(wal->fds[i], &vs[i], sizeof(vs[i]), 0);
    }

    const bool two = vs[0] && vs[1]; // 两个值均非零
    const u64 v0 = msstz_version(xdb->z);
    debug_assert(v0);
    logger_printf(xdb->logfd, "%s wal1 %lu wal2 %lu zv %lu\n", __func__, vs[0], vs[1], v0);

    // 优先恢复 fds[1]，必要时再恢复 fds[0]；之后继续使用 fds[0]，因为它可能仍有未写满的数据
    if (vs[0] < vs[1]) { // 交换
        logger_printf(xdb->logfd, "%s use wal2 %lu\n", __func__, vs[1]);
        wal->version = vs[1];
        const int fd1 = wal->fds[0];
        wal->fds[0]= wal->fds[1];
        wal->fds[1] = fd1;
        wring_update_fd(wal->wring, wal->fds[0]);
    } else {
        logger_printf(xdb->logfd, "%s use wal1 %lu\n", __func__, vs[0]);
        wal->version = vs[0];
    }

    debug_assert(wal->wring && wal->buf);

    if (two) { // 立即执行压缩
        if (vs[0] == vs[1])
            debug_die(); // 两个 WAL 的版本号必须不同
        const u64 r1 = xdb_recover_fd(xdb, wal->fds[1]); // 扫描较旧的 WAL
        const u64 r0 = xdb_recover_fd(xdb, wal->fds[0]); // 扫描较新的 WAL
        // 压缩全部数据，不拒绝任何记录
        msstz_comp(xdb->z, imt_api, xdb->mt1, xdb->nr_workers, xdb->co_per_worker, 0);
        // 此时新版本已安全可用
        ftruncate(wal->fds[1], 0);
        fdatasync(wal->fds[1]);
        ftruncate(wal->fds[0], 0);
        fdatasync(wal->fds[0]);
        imt_api->clean(xdb->mt1);
        xdb->mtsz = 0;
        // 重新初始化
        const u64 v1 = msstz_version(xdb->z);
        memcpy(wal->buf, &v1, sizeof(v1));
        wal->bufoff = sizeof(v1);
        wal->version = v1;
        logger_printf(xdb->logfd, "%s wal comp zv0 %lu zv1 %lu rec %lu %lu mtsz %lu fd0 %d\n",
                      __func__, v0, v1, r1, r0, xdb->mtsz, wal->fds[0]);
    } else { // 存在一个或不存在有效日志
        const u64 rsize = xdb_recover_fd(xdb, wal->fds[0]);
        if (rsize == 0) { // 为空 WAL 文件设置版本号
            memcpy(wal->buf, &v0, sizeof(v0));
            wal->bufoff = sizeof(v0);
            wal->version = v0;
            logger_printf(xdb->logfd, "%s wal empty v %lu mtsz %lu fd %d\n", __func__, v0, xdb->mtsz, wal->fds[0]);
        } else { // 复用现有 WAL
            // 仅存在一个 WAL：其版本号不大于 Z 的版本号
            if (wal->version > v0)
                debug_die();
            // woff 必须满足对齐要求
            wal->woff = bits_round_up(rsize, 12);
            if (wal->woff > rsize) { // 使用零字节填充间隙
                const u64 nr = wal->woff - rsize;
                u8 zeroes[PGSZ];
                memset(zeroes, 0, nr);
                pwrite(wal->fds[0], zeroes, nr, (off_t)rsize);
                fdatasync(wal->fds[0]);
            }
            logger_printf(xdb->logfd, "%s wal rsize %lu woff %lu mtsz %lu fd %d\n", __func__, rsize, wal->woff, xdb->mtsz, wal->fds[0]);
        }
        ftruncate(wal->fds[1], 0); // 无论如何都截断第二个 WAL
        fdatasync(wal->fds[1]);
    }
    wal->soff = wal->woff;
}
// }}} 恢复流程区

// 打开与关闭流程区 {{{
struct xdb *
xdb_open(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb, const size_t wal_size_mb,
         const bool ckeys, const bool tags, const u32 nr_workers, const u32 co_per_worker, const char * const worker_cores)
{
    mkdir(dir, 00755);
    struct xdb * const xdb = yalloc(sizeof(*xdb));
    if (!xdb)
        return NULL;

    memset(xdb, 0, sizeof(*xdb));

    const struct kvmap_mm mm_mt = { .in = kvmap_mm_in_noop, .out = kvmap_mm_out_noop, .free = kvmap_mm_free_free};

    xdb->mt1 = wormhole_create(&mm_mt);
    xdb->mt2 = wormhole_create(&mm_mt);

    xdb->mt_views[0] = (struct mt_pair){.wmt = xdb->mt1, .next = &xdb->mt_views[1]};
    xdb->mt_views[1] = (struct mt_pair){.wmt = xdb->mt2, .imt = xdb->mt1, .next = &xdb->mt_views[2]};

    xdb->mt_views[2] = (struct mt_pair){.wmt = xdb->mt2, .next = &xdb->mt_views[3]};
    xdb->mt_views[3] = (struct mt_pair){.wmt = xdb->mt1, .imt = xdb->mt2, .next = &xdb->mt_views[0]};
    xdb->mt_view = xdb->mt_views; // 第 0 个版本

    xdb->z = msstz_open(dir, cache_size_mb, ckeys, tags);
    xdb->qsbr = qsbr_create();

    // 仅输出警告
    if ((mt_size_mb * 2) > wal_size_mb)
        fprintf(stderr, "%s wal_size < mt_size*2\n", __func__);

    // 大小
    xdb->max_mtsz = mt_size_mb << 20;
    xdb->wal.maxsz = wal_size_mb << 20;
    xdb->max_rejsz = xdb->max_mtsz >> XDB_REJECT_SIZE_SHIFT;

    spinlock_init(&xdb->lock);
    xdb->nr_workers = nr_workers; // 内部并行度
    xdb->co_per_worker = co_per_worker;
    xdb->worker_cores = strdup(worker_cores);
    xdb->logfd = msstz_logfd(xdb->z);
    xdb->running = true;
    xdb->causal_meta_current_wmt_processed = true; // 数据库打开时，默认当前可写 MemTable 尚无需要提交的未处理页面。

    const bool wal_ok = wal_open(&xdb->wal, dir);
    const bool all_ok = xdb->mt1 && xdb->mt2 && xdb->z && xdb->qsbr && wal_ok;
    if (all_ok) {
        xdb_wal_recover(xdb); // 恢复过程未发现错误
        if (xdb->mtsz > 0)
            xdb->causal_meta_current_wmt_processed = false; // 若 WAL 恢复出尚未落盘的数据，则关闭前仍需将其提交给元数据模块。

        // 启动主压缩工作线程
        pthread_create(&xdb->comp_pid, NULL, xdb_compaction_worker, xdb); // 此处预期返回 0
        return xdb;
    } else { // 打开失败
        if (xdb->mt1)
            wmt_api->destroy(xdb->mt1);
        if (xdb->mt2)
            wmt_api->destroy(xdb->mt2);
        if (xdb->z)
            msstz_destroy(xdb->z);
        if (xdb->qsbr)
            qsbr_destroy(xdb->qsbr);
        if (wal_ok)
            wal_close(&xdb->wal);
        free(xdb);
        return NULL;
    }
}

// 销毁数据库
void
xdb_close(struct xdb * xdb)
{
    xdb->running = false;
    pthread_join(xdb->comp_pid, NULL);

    // 关闭数据库前，处理尚未达到刷盘阈值的当前 MemTable，并持久化相应元数据。
    xdb_causal_meta_flush_current_wmt(xdb);
    causal_meta_on_xdb_close();

    // 假定所有用户线程均已退出
    qsbr_destroy(xdb->qsbr);

    msstz_destroy(xdb->z);
    wal_close(&xdb->wal);
    wmt_api->destroy(xdb->mt1);
    wmt_api->destroy(xdb->mt2);
    free(xdb->worker_cores);
    free(xdb);
}
// }}} 打开与关闭流程区

// 查询与探测流程区 {{{
struct xdb_get_info {
    struct kv * out;
    struct kv * ret;
};

static void
xdb_inp_get(struct kv * const kv, void * const priv)
{
    // 读取该键时复制数据
    // 避免 get 返回后引用的数据发生一致性问题
    struct xdb_get_info * const info = (typeof(info))priv;
    if (kv && kv->vlen != SST_VLEN_TS) {
        info->ret = kvmap_mm_out_ts(kv, info->out);
    } else {
        info->ret = NULL;
    }
}

struct kv *
xdb_get(struct xdb_ref * const ref, const struct kref * const kref, struct kv * const out)
{
    xdb_ref_update_version(ref);
    xdb_ref_enter(ref);

    // 可写 MemTable（WMT）
    struct xdb_get_info info = {out, NULL};
    if (wmt_api->inpr(ref->wmt_ref, kref, xdb_inp_get, &info)) {
        xdb_ref_leave(ref);
        return info.ret;
    }
    xdb_ref_leave(ref);

    // 不可变 MemTable（IMT）
    if (ref->imt_ref) {
        if (imt_api->inpr(ref->imt_ref, kref, xdb_inp_get, &info))
            return info.ret;
    }
    // 日志中不存在该键，可能位于 SST 文件中
    return msstv_get_ts(ref->vref, kref, out);
}

static void
xdb_inp_probe(struct kv * const kv, void * const priv)
{
    // 读取该键时复制数据
    // 避免 get 返回后引用的数据发生一致性问题
    *(bool *)priv = kv && (kv->vlen != SST_VLEN_TS);
}

bool
xdb_probe(struct xdb_ref * const ref, const struct kref * const kref)
{
    xdb_ref_update_version(ref);
    xdb_ref_enter(ref);

    bool is_valid;
    if (wmt_api->inpr(ref->wmt_ref, kref, xdb_inp_probe, &is_valid)) {
        xdb_ref_leave(ref);
        return is_valid;
    }
    xdb_ref_leave(ref);

    if (ref->imt_ref) {
        if (imt_api->inpr(ref->imt_ref, kref, xdb_inp_probe, &is_valid))
            return is_valid;
    }
    return msstv_probe_ts(ref->vref, kref);
}
// }}} 查询与探测流程区

// 写入与删除流程区 {{{
// 此处处理路径较长
static void
xdb_write_enter(struct xdb_ref * const ref)
{
    struct xdb * const xdb = ref->xdb;
    while (xdb_mt_wal_full(xdb)) {
        xdb_ref_update_version(ref);
        usleep(10);
    }
}

struct xdb_mt_merge_ctx {
    struct kv * newkv;
    struct xdb * xdb;
    struct mt_pair * mt_view;
    bool success;
};

// kv_merge_func 回调
// 调用时持有锁
static struct kv *
xdb_mt_update_func(struct kv * const kv0, void * const priv)
{
    struct xdb_mt_merge_ctx * const ctx = priv;
    struct xdb * const xdb = ctx->xdb;
    const size_t newsz = sst_kv_size(ctx->newkv);
    const size_t oldsz = kv0 ? sst_kv_size(kv0) : 0;
    const size_t diffsz = newsz - oldsz;

    xdb_lock(xdb);
    if (unlikely(xdb->mt_view != ctx->mt_view)) {
        // 中止本次操作
        xdb_unlock(xdb);
        return NULL;
    }
    debug_assert(xdb->mtsz >= oldsz);
    xdb->mtsz += diffsz;
    xdb->wal.write_user += newsz;
    wal_append(&xdb->wal, ctx->newkv);
    xdb->causal_meta_current_wmt_processed = false; // 当前可写 MemTable 已写入新数据；后续关闭数据库或显式执行 flush 时，需要将其提交给元数据模块。

    xdb_unlock(xdb);
    ctx->success = true;
    return ctx->newkv;
}

static bool
xdb_update(struct xdb_ref * const ref, const struct kref * const kref, struct kv * const newkv)
{
    debug_assert(kref && newkv);
    xdb_write_enter(ref);

    struct xdb_mt_merge_ctx ctx = {newkv, ref->xdb, NULL, false};
    bool s;
    do {
        xdb_ref_update_version(ref);
        xdb_ref_enter(ref);
        ctx.mt_view = ref->mt_view;
        s = wmt_api->merge(ref->wmt_ref, kref, xdb_mt_update_func, &ctx);
        xdb_ref_leave(ref);
    } while (s && !ctx.success);
    return s;
}

bool
xdb_put(struct xdb_ref * const ref, const struct kv * const kv)
{
    struct kv * const newkv = xdb_dup_kv(kv);
    if (!newkv)
        return false;

    struct kref kref;
    kref_ref_kv(&kref, kv);
    return xdb_update(ref, &kref, newkv);
}

bool
xdb_del(struct xdb_ref * const ref, const struct kref * const kref)
{
    struct kv * const ts_kv = xdb_new_ts(kref);
    if (!ts_kv)
        return false;

    return xdb_update(ref, kref, ts_kv);
}

void
xdb_sync(struct xdb_ref * const ref)
{
    struct xdb * const xdb = ref->xdb;
    xdb_lock(xdb);
    wal_flush_sync_wait(&xdb->wal);
    xdb_unlock(xdb);
}
// }}} 写入与删除流程区

// 合并操作区 {{{
// 返回的 kv 由调用方负责释放
static struct kv *
xdb_merge_get_old(struct xdb_ref * const ref, const struct kref * const kref)
{
    struct xdb_get_info info = {NULL, NULL}; // 不输出额外结果
    // 不可变 MemTable（IMT）
    if (ref->imt_ref) {
        if (imt_api->inpr(ref->imt_ref, kref, xdb_inp_get, &info))
            return info.ret;
    }
    // 日志中不存在该键，可能位于 SST 文件中
    struct kv * const ret = msstv_get_ts(ref->vref, kref, NULL);
    if (ret)
        ret->hash = kv_crc32c_extend(kref->hash32);
    return ret;
}

struct xdb_rmw_ctx {
    struct xdb_mt_merge_ctx mt_ctx; // newkv、xdb、mt_view 和 success 字段
    kv_merge_func uf;
    void * const priv;
    struct kv * oldkv; // 仅 func2 使用
    bool merged; // 仅 func1 使用
};

// kv_merge_func 回调
static struct kv *
xdb_merge_merge_func(struct kv * const kv0, void * const priv)
{
    struct xdb_rmw_ctx * const ctx = priv;
    struct kv * const oldkv = kv0 ? kv0 : ctx->oldkv;
    struct kv * const ukv = ctx->uf(oldkv, ctx->priv);
    if (ukv == NULL) { // 只读字段
        ctx->merged = true;
        return NULL;
    }

    // 条件允许时复用 kv0
    struct kv * const newkv = (ukv != kv0) ? xdb_dup_kv(ukv) : ukv;
    ctx->mt_ctx.newkv = newkv;

    struct kv * const ret = xdb_mt_update_func(kv0, &ctx->mt_ctx);
    if (ctx->mt_ctx.success)
        ctx->merged = true;
    else if (ukv != kv0)
        free(newkv);

    return ret;
}

// 仅当找到目标键时执行合并
// kv_merge_func 回调
static struct kv *
xdb_merge_merge_func1(struct kv * const kv0, void * const priv)
{
    struct xdb_rmw_ctx * const ctx = priv;
    if (kv0 == NULL) { // 未找到目标键：若存在旧值，则尝试与其合并后返回
        ctx->mt_ctx.success = true; // 返回时 merged 为 false
        return NULL;
    }

    return xdb_merge_merge_func(kv0, priv);
}

bool
xdb_merge(struct xdb_ref * const ref, const struct kref * const kref, kv_merge_func uf, void * const priv)
{
    debug_assert(kref && uf);
    xdb_write_enter(ref);

    struct xdb_rmw_ctx ctx = {.mt_ctx = {.xdb = ref->xdb}, .uf = uf, .priv = priv};

    bool s;
    do {
        xdb_ref_update_version(ref);
        xdb_ref_enter(ref);
        ctx.mt_ctx.mt_view = ref->mt_view;
        s = wmt_api->merge(ref->wmt_ref, kref, xdb_merge_merge_func1, &ctx);
        xdb_ref_leave(ref);
    } while (s && !ctx.mt_ctx.success);

    if (ctx.merged || (!s))
        return s;
    // WMT 中不存在该键
    ctx.mt_ctx.success = false;

    do {
        xdb_ref_update_version(ref);
        ctx.oldkv = xdb_merge_get_old(ref, kref);
        xdb_ref_enter(ref);
        ctx.mt_ctx.mt_view = ref->mt_view;
        s = wmt_api->merge(ref->wmt_ref, kref, xdb_merge_merge_func, &ctx);
        xdb_ref_leave(ref);
        free(ctx.oldkv); // 该指针可能为 NULL
    } while (s && !ctx.merged);
    return s;
}
// }}} 合并操作区

// 迭代器流程区 {{{
static void
xdb_iter_miter_ref(struct xdb_iter * const iter)
{
    struct xdb_ref * const ref = iter->db_ref;
    iter->mt_view = ref->mt_view; // 记录 miter 使用的 mt_view

    miter_add_ref(iter->miter, &kvmap_api_msstv_ts, ref->vref);

    if (ref->imt_ref)
        miter_add_ref(iter->miter, imt_api, ref->imt_ref);

    miter_add_ref(iter->miter, wmt_api, ref->wmt_ref);
}

static void
xdb_iter_update_version(struct xdb_iter * const iter)
{
    struct xdb_ref * const ref = iter->db_ref;
    if ((ref->mt_view == ref->xdb->mt_view) && (iter->mt_view == ref->mt_view))
        return;

    miter_clean(iter->miter);
    xdb_ref_update_version(ref);
    xdb_iter_miter_ref(iter);
    // 获取新的引用
}

struct xdb_iter *
xdb_iter_create(struct xdb_ref * const ref)
{
    struct xdb_iter * const iter = calloc(1, sizeof(*iter));
    iter->miter = miter_create();
    iter->db_ref = ref;

    xdb_ref_update_version(ref);
    xdb_iter_miter_ref(iter);
    xdb_iter_park(iter);
    return iter;
}

static void
xdb_iter_skip_ts(struct xdb_iter * const iter)
{
    struct kvref kvref;
    do {
        if (miter_kvref(iter->miter, &kvref) == false)
            return;
        if (kvref.hdr.vlen != SST_VLEN_TS)
            break;
        miter_skip_unique(iter->miter);
    } while (true);
}

void
xdb_iter_park(struct xdb_iter * const iter)
{
    miter_park(iter->miter);

    if (iter->coq_parked) {
        coq_install(iter->coq_parked);
        iter->coq_parked = NULL;
    }
}

void
xdb_iter_seek(struct xdb_iter * const iter, const struct kref * const key)
{
    xdb_iter_update_version(iter);

    struct coq * const coq = coq_current();
    if (coq) {
        iter->coq_parked = coq;
        coq_uninstall();
    }

    miter_seek(iter->miter, key);
    xdb_iter_skip_ts(iter);
}

bool
xdb_iter_valid(struct xdb_iter * const iter)
{
    return miter_valid(iter->miter);
}

// 假定调用 peek 前已经执行 valid
struct kv *
xdb_iter_peek(struct xdb_iter * const iter, struct kv * const out)
{
    struct kvref kvref;
    if (!miter_kvref(iter->miter, &kvref))
        return NULL;

    // 此处不应出现 TS
    debug_assert(kvref.hdr.vlen != SST_VLEN_TS);
    return sst_kvref_dup2_kv(&kvref, out);
}

bool
xdb_iter_kref(struct xdb_iter * const iter, struct kref * const kref)
{
    return miter_kref(iter->miter, kref);
}

bool
xdb_iter_kvref(struct xdb_iter * const iter, struct kvref * const kvref)
{
    return miter_kvref(iter->miter, kvref);
}

void
xdb_iter_skip1(struct xdb_iter * const iter)
{
    miter_skip_unique(iter->miter);
    xdb_iter_skip_ts(iter);
}

void
xdb_iter_skip(struct xdb_iter * const iter, const u32 n)
{
    for (u32 i = 0; i < n; i++) {
        miter_skip_unique(iter->miter);
        xdb_iter_skip_ts(iter);
    }
}

struct kv *
xdb_iter_next(struct xdb_iter * const iter, struct kv * const out)
{
    struct kv * const kv = xdb_iter_peek(iter, out);
    xdb_iter_skip1(iter);
    return kv;
}

void
xdb_iter_destroy(struct xdb_iter * const iter)
{
    miter_destroy(iter->miter);

    if (iter->coq_parked) {
        coq_install(iter->coq_parked);
        iter->coq_parked = NULL;
    }

    free(iter);
}
// }}} 迭代器流程区

// API 接口区 {{{
const struct kvmap_api kvmap_api_xdb = {
        .hashkey = true,
        .ordered = true,
        .threadsafe = true,
        .unique = true,
        .get = (void*)xdb_get,
        .probe = (void*)xdb_probe,
        .put = (void*)xdb_put,
        .del = (void*)xdb_del,
        .merge = (void*)xdb_merge,
        .sync = (void*)xdb_sync,
        .ref = (void*)xdb_ref,
        .unref = (void*)xdb_unref,
        .destroy = (void*)xdb_close,

        .iter_create = (void*)xdb_iter_create,
        .iter_seek = (void*)xdb_iter_seek,
        .iter_valid = (void*)xdb_iter_valid,
        .iter_peek = (void*)xdb_iter_peek,
        .iter_kref = (void*)xdb_iter_kref,
        .iter_kvref = (void*)xdb_iter_kvref,
        .iter_skip1 = (void*)xdb_iter_skip1,
        .iter_skip = (void*)xdb_iter_skip,
        .iter_next = (void*)xdb_iter_next,
        .iter_park = (void*)xdb_iter_park,
        .iter_destroy = (void*)xdb_iter_destroy,
};

static void *
xdb_kvmap_api_create(const char * const name, const struct kvmap_mm * const mm, char ** const args)
{
    (void)mm;
    if (!strcmp(name, "xdb")) {
        const char * const dir = args[0];
        const size_t cache_size_mb = a2u64(args[1]);
        const size_t mt_size_mb = a2u64(args[2]);
        const size_t wal_size_mb = (strcmp(args[3], "auto") == 0) ? (mt_size_mb << 1) : a2u64(args[3]);
        const bool ckeys = args[4][0] != '0';
        const bool tags = args[5][0] != '0';
        const u32 nr_workers = (strcmp(args[6], "auto") == 0) ? 4 : a2u32(args[6]);
        const u32 co_per_worker = (strcmp(args[7], "auto") == 0) ? (ckeys ? 1 : 4) : a2u32(args[7]);
        const char * const worker_cores = args[8];
        return xdb_open(dir, cache_size_mb, mt_size_mb, wal_size_mb, ckeys, tags, nr_workers, co_per_worker, worker_cores);

    } else if (!strcmp(name, "xdbauto")) {
        const char * const dir = args[0];
        const size_t cache_size_mb = a2u64(args[1]);
        const size_t mt_size_mb = a2u64(args[2]);
        const bool tags = args[3][0] != '0';
        return xdb_open(dir, cache_size_mb, mt_size_mb, mt_size_mb << 1, true, tags, 4, 1, "auto");
    }
    return NULL;
}

__attribute__((constructor))
static void
xdb_kvmap_api_init(void)
{
    kvmap_api_register(9, "xdb", "<path> <cache-mb> <mt-mb> <wal-mb/auto> <ckeys(0/1)> <tags(0/1)>"
                                 " <nr-workers/auto> <co-per-worker/auto> <worker-cores/auto/dont>",
                       xdb_kvmap_api_create, &kvmap_api_xdb);

    kvmap_api_register(4, "xdbauto", "<path> <cache-mb> <mt-mb> <tags(0/1)>",
                       xdb_kvmap_api_create, &kvmap_api_xdb);
}
// }}}

// RemixDB 接口区 {{{
// 默认模式：生成 ckeys 和 tags，执行速度较快，但会额外占用少量内存和磁盘空间
// 如需更多配置项，请使用 xdb_open()
struct xdb *
remixdb_open(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb, const bool tags)
{
    return xdb_open(dir, cache_size_mb, mt_size_mb, mt_size_mb << 1, true, tags, 4, 1, "auto");
}

// 该模式的写放大（WA）略低，磁盘占用也更少；
// 但当工作负载的写入局部性较差时，压缩过程可能变慢
// 同时会禁用 hash-tags，因此点查询速度会明显下降
// 仅在磁盘空间确实非常紧张时才建议使用该模式
struct xdb *
remixdb_open_compact(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb)
{
    return xdb_open(dir, cache_size_mb, mt_size_mb, mt_size_mb << 1, false, false, 4, 4, "auto");
}

struct xdb_ref *
remixdb_ref(struct xdb * const xdb)
{
    return xdb_ref(xdb);
}

void
remixdb_unref(struct xdb_ref * const ref)
{
    (void)xdb_unref(ref);
}

void
remixdb_close(struct xdb * const xdb)
{
    xdb_close(xdb);
}

void
remixdb_causal_meta_flush(struct xdb * const xdb)
{
    // 供实验代码在数据库保持打开时显式提交当前 MemTable，并持久化元数据，以便随后立即执行查询。
    xdb_causal_meta_flush_current_wmt(xdb);
    causal_meta_on_xdb_close();
}

bool
remixdb_put(struct xdb_ref * const ref, const void * const kbuf, const u32 klen,
            const void * const vbuf, const u32 vlen)
{
    // TODO：较大的 KV 应考虑单独存储，并在 xdb 中保存间接引用
    if ((klen + vlen) > 65500)
        return false;

    struct kv * const newkv = kv_create(kbuf, klen, vbuf, vlen);
    if (!newkv)
        return false;

    struct kref kref;
    kref_ref_kv(&kref, newkv);
    return xdb_update(ref, &kref, newkv);
}

bool
remixdb_del(struct xdb_ref * const ref, const void * const kbuf, const u32 klen)
{
    struct kref kref;
    kref_ref_hash32(&kref, kbuf, klen);

    struct kv * const ts_kv = xdb_new_ts(&kref);
    if (!ts_kv)
        return false;

    return xdb_update(ref, &kref, ts_kv);
}

// 检查 Wormhole 中是否存在该键
bool
remixdb_probe(struct xdb_ref * const ref, const void * const kbuf, const u32 klen)
{
    struct kref kref;
    kref_ref_hash32(&kref, kbuf, klen);
    return xdb_probe(ref, &kref);
}

struct remixdb_get_info { void * vbuf_out; u32 * vlen_out; };

static void
remixdb_inp_get(struct kv * kv, void * priv)
{
    // 读取该键时复制数据
    // 避免 get 返回后引用的数据发生一致性问题
    if (kv) {
        struct remixdb_get_info * const info = (typeof(info))priv;
        *info->vlen_out = kv->vlen; // 复制原始 vlen 值
        if (kv->vlen != SST_VLEN_TS)
            memcpy(info->vbuf_out, kv_vptr_c(kv), kv->vlen & SST_VLEN_MASK);
    }
}

bool
remixdb_get(struct xdb_ref * const ref, const void * const kbuf, const u32 klen,
            void * const vbuf_out, u32 * const vlen_out)
{
    struct kref kref;
    kref_ref_hash32(&kref, kbuf, klen);

    xdb_ref_update_version(ref);
    xdb_ref_enter(ref);

    // 可写 MemTable（WMT）
    struct remixdb_get_info info = {vbuf_out, vlen_out};
    if (wmt_api->inpr(ref->wmt_ref, &kref, remixdb_inp_get, &info)) {
        xdb_ref_leave(ref);
        return (*vlen_out) != SST_VLEN_TS;
    }
    xdb_ref_leave(ref);

    // 不可变 MemTable（IMT）
    if (ref->imt_ref) {
        if (imt_api->inpr(ref->imt_ref, &kref, remixdb_inp_get, &info))
            return (*vlen_out) != SST_VLEN_TS;
    }
    // 日志中不存在该键，可能位于 SST 文件中
    return msstv_get_value_ts(ref->vref, &kref, vbuf_out, vlen_out);
}

void
remixdb_sync(struct xdb_ref * const ref)
{
    return xdb_sync(ref);
}

struct xdb_iter *
remixdb_iter_create(struct xdb_ref * const ref)
{
    return xdb_iter_create(ref);
}

void
remixdb_iter_seek(struct xdb_iter * const iter, const void * const kbuf, const u32 klen)
{
    struct kref kref;
    kref_ref_hash32(&kref, kbuf, klen);
    xdb_iter_seek(iter, &kref);
}

bool
remixdb_iter_valid(struct xdb_iter * const iter)
{
    return xdb_iter_valid(iter);
}

bool
remixdb_iter_peek(struct xdb_iter * const iter,
                  void * const kbuf_out, u32 * const klen_out,
                  void * const vbuf_out, u32 * const vlen_out)
{
    struct kvref kvref;
    if (!miter_kvref(iter->miter, &kvref))
        return false;

    // 此处不应出现 TS
    debug_assert(kvref.hdr.vlen != SST_VLEN_TS);
    if (kbuf_out) {
        const u32 klen = kvref.hdr.klen;
        memcpy(kbuf_out, kvref.kptr, klen);
        *klen_out = klen;
    }

    if (vbuf_out) {
        const u32 vlen = kvref.hdr.vlen & SST_VLEN_MASK;
        memcpy(vbuf_out, kvref.vptr, vlen);
        *vlen_out = vlen;
    }

    return true;
}

void
remixdb_iter_skip1(struct xdb_iter * const iter)
{
    xdb_iter_skip1(iter);
}

void
remixdb_iter_skip(struct xdb_iter * const iter, const u32 nr)
{
    xdb_iter_skip(iter, nr);
}

void
remixdb_iter_park(struct xdb_iter * const iter)
{
    xdb_iter_park(iter);
}

void
remixdb_iter_destroy(struct xdb_iter * const iter)
{
    xdb_iter_destroy(iter);
}
// }}} RemixDB 接口区

// fdm: marker
