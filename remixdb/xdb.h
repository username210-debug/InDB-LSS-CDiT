/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

/*
 * 本文件声明 RemixDB 的数据库操作接口及元数据维护所需的扩展接口。
 */

#pragma once

#include "lib.h"
#include "kv.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xdb;
struct xdb_ref;
struct xdb_iter;

// xdb 接口区 {{{
extern struct xdb *
xdb_open(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb, const size_t wal_size_mb,
         const bool ckeys, const bool tags, const u32 nr_workers, const u32 co_per_worker, const char * const worker_cores);

extern void
xdb_close(struct xdb * const xdb);

// kvmap_api 接口
extern struct xdb_ref *
xdb_ref(struct xdb * const xdb);

extern struct xdb*
xdb_unref(struct xdb_ref * const ref);

extern struct kv *
xdb_get(struct xdb_ref * const ref, const struct kref * const kref, struct kv * const out);

extern bool
xdb_probe(struct xdb_ref * const ref, const struct kref * const kref);

extern bool
xdb_put(struct xdb_ref * const ref, const struct kv * const kv);

extern bool
xdb_del(struct xdb_ref * const ref, const struct kref * const kref);

extern void
xdb_sync(struct xdb_ref * const ref);

// 又称原子读-改-写（Atomic Read-Modify-Write）
// 合并操作可能因内存分配失败而不产生任何修改
// uf() 可能因操作中止与重试而被多次调用，这属于正常流程
// 只有最后一次成功的调用会真正生效
// 除最后一次调用 uf() 返回的 kv 外，其余返回值都会被忽略
// uf() 分配的内存必须由调用方在 xdb_merge() 返回后释放
// 当 kv0 非空时，uf() 可以通过直接返回 kv0 执行原位更新
// 若 kv0 来自磁盘分区而非 MemTable，原位更新仍可能触发一次 MemTable 插入
extern bool
xdb_merge(struct xdb_ref * const ref, const struct kref * const kref, kv_merge_func uf, void * const priv);

// 迭代器接口
extern struct xdb_iter *
xdb_iter_create(struct xdb_ref * const ref);

extern void
xdb_iter_park(struct xdb_iter * const iter);

extern void
xdb_iter_seek(struct xdb_iter * const iter, const struct kref * const key);

extern bool
xdb_iter_valid(struct xdb_iter * const iter);

extern struct kv *
xdb_iter_peek(struct xdb_iter * const iter, struct kv * const out);

extern bool
xdb_iter_kref(struct xdb_iter * const iter, struct kref * const kref);

extern bool
xdb_iter_kvref(struct xdb_iter * const iter, struct kvref * const kvref);

extern void
xdb_iter_skip1(struct xdb_iter * const iter);

extern void
xdb_iter_skip(struct xdb_iter * const iter, u32 n);

extern struct kv*
xdb_iter_next(struct xdb_iter * const iter, struct kv * const out);

extern void
xdb_iter_destroy(struct xdb_iter * const iter);

extern const struct kvmap_api kvmap_api_xdb;
// }}} xdb 接口区

// RemixDB 接口区 {{{
extern struct xdb *
remixdb_open(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb, const bool tags);

extern struct xdb *
remixdb_open_compact(const char * const dir, const size_t cache_size_mb, const size_t mt_size_mb);

extern struct xdb_ref *
remixdb_ref(struct xdb * const xdb);

extern void
remixdb_unref(struct xdb_ref * const ref);

extern void
remixdb_close(struct xdb * const xdb);

extern bool
remixdb_put(struct xdb_ref * const ref, const void * const kbuf, const u32 klen,
            const void * const vbuf, const u32 vlen);

extern bool
remixdb_del(struct xdb_ref * const ref, const void * const kbuf, const u32 klen);

extern bool
remixdb_probe(struct xdb_ref * const ref, const void * const kbuf, const u32 klen);

extern bool
remixdb_get(struct xdb_ref * const ref, const void * const kbuf, const u32 klen,
            void * const vbuf_out, u32 * const vlen_out);

extern void
remixdb_sync(struct xdb_ref * const ref);

extern void
remixdb_causal_meta_flush(struct xdb * const xdb);

extern struct xdb_iter *
remixdb_iter_create(struct xdb_ref * const ref);

extern void
remixdb_iter_seek(struct xdb_iter * const iter, const void * const kbuf, const u32 klen);

extern bool
remixdb_iter_valid(struct xdb_iter * const iter);

extern bool
remixdb_iter_peek(struct xdb_iter * const iter,
                  void * const kbuf_out, u32 * const klen_out,
                  void * const vbuf_out, u32 * const vlen_out);

extern void
remixdb_iter_skip1(struct xdb_iter * const iter);

extern void
remixdb_iter_skip(struct xdb_iter * const iter, const u32 nr);

extern void
remixdb_iter_park(struct xdb_iter * const iter);

extern void
remixdb_iter_destroy(struct xdb_iter * const iter);
// }}} RemixDB 接口区

#ifdef __cplusplus
}
#endif
// vim:fdm=marker
