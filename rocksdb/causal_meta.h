#pragma once

/*
 * 本文件声明 RocksDB 侧元数据模块的配置、统计、页面维护、持久化和查询接口，并提供与 RocksDB 公开扩展点对接的声明。
 */


#include "cdit_module.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#ifndef CAUSAL_META_NO_ROCKSDB
namespace rocksdb {
struct Options;
}
#endif

// causal_meta 对时间戳本身不做任何格式假设；key 用于精确定位，label 仅用于日志显示。
struct CausalMetaTimestampInfo {
    std::string key;
    std::string label;
};

// 单个时间序列数据集的通用配置。
// ordered_timestamps 必须按真实时间顺序给出“本次允许插入范围内”的所有唯一时间戳。
// sampling_step 只影响因果推断采样；所有原始行仍可照常写入 RocksDB。
struct CausalMetaDatasetConfig {
    std::string dataset_id;
    std::string display_name;
    std::vector<std::string> variable_names;
    std::vector<CausalMetaTimestampInfo> ordered_timestamps;

    int sampling_step = 1;
    int window_length = 365;
    int tau = 5;
    double alpha = 0.01;
    bool infer_self = false;
    bool use_covariance_cache_cdit = false;

    // 1-based CDiT 列号；空表示全部列。
    std::vector<int> source_columns;
    std::vector<int> target_columns;
};

struct CausalMetaCounters {
    long long total_pages = 0;
    long long complementary_pages = 0;
    long long overlapped_pages = 0;
    long long inferred_windows = 0;
    long long original_complete_windows = 0;
    long long final_metadata_entries = 0;

    // 这里记录“元数据功能真正执行工作的活动墙钟区间”之和：
    // Flush 捕获、窗口/CDiT/C/B 维护、要求开启时的页面日志、持久化；
    // 不包含异步队列等待、等待 MemTable 满页、写停顿等无关阻塞时间。
    double metadata_seconds = 0.0;
    double flush_capture_seconds = 0.0;
    double worker_seconds = 0.0;
    double persistence_seconds = 0.0;
};

struct CausalMetaQuerySegment {
    std::string dataset_id;
    std::string start_key;
    std::string end_key;
    std::string start_label;
    std::string end_label;
    std::vector<std::string> variable_names;
    CDiTResult graph;
};

// 全局生命周期/开关。enabled 默认 true。
void causal_meta_reset_all();
void causal_meta_set_enabled_cpp(bool enabled);
bool causal_meta_is_enabled();

// 注册数据集。注册必须发生在该数据集开始写入 RocksDB 之前。
bool causal_meta_register_dataset(const CausalMetaDatasetConfig& cfg,
                                  std::string* error = nullptr);
bool causal_meta_has_dataset(const std::string& dataset_id);
std::vector<std::string> causal_meta_registered_dataset_ids();

// 元数据持久化目录，例如 meta_file/basin_experiment。
void causal_meta_set_metadata_dir(const std::string& dir);
std::string causal_meta_get_metadata_dir();

// 页面过程日志只在 true 时才真正组织页面/C/B 字符串，false 时不会做“隐形格式化”。
void causal_meta_set_page_logging(bool enabled);
void causal_meta_set_log_sink(void (*sink)(const char*, void*), void* userdata);
void causal_meta_clear_log_sink();

#ifndef CAUSAL_META_NO_ROCKSDB
// 将通用元数据模块挂到 RocksDB 的公开 Flush 建表扩展点。
// 不修改 RocksDB 原始源码；重复调用同一 Options 也只附加一次本模块监听器/collector。
bool causal_meta_attach_to_rocksdb_options(rocksdb::Options* options,
                                           std::string* error = nullptr);
#endif

// 等待已经成功 Flush 并入队的元数据工作完成。
void causal_meta_wait_for_idle();

// 将 C/B 落盘。内部会先等待异步元数据工作完成。
bool causal_meta_persist_all(std::string* error = nullptr);

// 通用的“关闭数据库前”动作：等待异步维护 + 持久化 C/B。
// 实验代码在 delete/reset DB 之前显式调用，避免在 RocksDB shutdown listener 中阻塞后台线程。
bool causal_meta_before_db_close(std::string* error = nullptr);

CausalMetaCounters causal_meta_get_counters();
double causal_meta_get_total_seconds();

std::string causal_meta_format_dataset_metadata(const std::string& dataset_id);
std::string causal_meta_format_dataset_buffers(const std::string& dataset_id);

// 范围查询从已经持久化的 C 文件读取，不访问 RocksDB 数据文件。
// query_start_key/query_end_key 为空分别表示 -∞/+∞。
// query_columns 为空表示全部列；否则按变量名投影有向边。
std::vector<CausalMetaQuerySegment> causal_meta_query_dataset_from_file(
        const std::string& metadata_dir,
        const std::string& dataset_id,
        const std::string& query_start_key,
        const std::string& query_end_key,
        const std::vector<std::string>& query_columns,
        std::string* error = nullptr);

std::string causal_meta_format_query_segment(
        const CausalMetaQuerySegment& seg,
        const std::vector<std::string>& preferred_order = std::vector<std::string>());

// 仅供单元测试/无需 RocksDB 的模块验证使用：模拟“一次 Flush 页”提交。
// 正常实验不调用它。
void causal_meta_submit_page_for_test(
        std::uint64_t page_id,
        const std::vector<std::pair<std::string, std::string>>& key_values);

extern "C" {
int causal_meta_enabled(void);
void causal_meta_set_enabled(int enabled);
}
