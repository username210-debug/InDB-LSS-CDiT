#pragma once

/*
 * 本文件声明 RemixDB 侧元数据维护接口，包括数据集注册、页面生命周期回调、持久化、状态统计和范围查询。
 */


#ifdef __cplusplus
#include "cdit_module.h"
#include <string>
#include <vector>

// 时间戳的存储键与展示标签分开保存：key 用于有序比较和精确定位，label 用于日志显示。
struct CausalMetaTimestampInfo {
    std::string key;
    std::string label;
};

// 单个数据集的窗口划分、分析参数、变量筛选和元数据存储配置。
struct CausalMetaDatasetConfig {
    std::string dataset_id;
    std::string display_name;
    std::vector<std::string> variable_names;
    std::vector<CausalMetaTimestampInfo> ordered_timestamps;
    int sampling_step = 1;
    int window_length = 365;
    int tau = 5;
    int delta = 2;
    double theta = 0.8;
    double alpha = 0.01;
    bool infer_self = false;
    bool ignore_self_in_merge = true;
    bool use_covariance_cache_cdit = false;
    std::vector<int> source_columns;
    std::vector<int> target_columns;
    std::string metadata_dir;
};

// 记录页面处理、窗口发现和元数据维护过程中的累计统计量。
struct CausalMetaCounters {
    long long total_pages = 0;
    long long complementary_pages = 0;
    long long overlapped_pages = 0;
    long long inferred_windows = 0;
    long long original_complete_windows = 0;
    long long final_metadata_entries = 0;
    double metadata_seconds = 0.0;
    double merge_seconds = 0.0;
};

// 范围查询返回的一个连续时间段及其对应的因果图。
struct CausalMetaQuerySegment {
    std::string dataset_id;
    std::string start_label;
    std::string end_label;
    CDiTResult graph;
};

// 生命周期与总开关。reset 会清空当前进程中的全部数据集状态。
void causal_meta_reset_all();
void causal_meta_set_enabled_cpp(bool enabled);
// 在写入该数据集的第一条记录前注册配置；同一数据集应只注册一次。
bool causal_meta_register_dataset(const CausalMetaDatasetConfig& cfg);
bool causal_meta_has_dataset(const std::string& dataset_id);
// 页面处理日志默认关闭；开启后才组织并输出页面与缓冲区状态文本。
void causal_meta_set_page_logging(bool enabled);
void causal_meta_set_log_sink(void (*sink)(const char*, void*), void* userdata);
void causal_meta_clear_log_sink();
void causal_meta_set_metadata_dir(const std::string& dir);
// 将当前内存中的元数据状态持久化到配置的目录。
void causal_meta_persist_all();
CausalMetaCounters causal_meta_get_counters();
double causal_meta_get_total_seconds();
std::string causal_meta_format_dataset_metadata(const std::string& dataset_id);
std::string causal_meta_format_dataset_buffers(const std::string& dataset_id);
std::vector<std::string> causal_meta_registered_dataset_ids();
// 从已持久化的元数据文件执行范围查询；空起止键分别表示无下界和无上界。
std::vector<CausalMetaQuerySegment> causal_meta_query_dataset_from_file(
        const std::string& metadata_dir,
        const std::string& dataset_id,
        const std::string& query_start_key,
        const std::string& query_end_key,
        const std::vector<std::string>& query_columns);
std::string causal_meta_format_query_segment(const CausalMetaQuerySegment& seg,
                                             const std::vector<std::string>& preferred_order = std::vector<std::string>());
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 以下 C 接口由 xdb.c 的写入、页面提交和关闭路径调用。
int causal_meta_xdb_enabled(void);
void causal_meta_set_enabled(int enabled);
void causal_meta_xdb_begin_page(void);
void causal_meta_xdb_add_kv(const void* kbuf, unsigned klen, const void* vbuf, unsigned vlen);
void causal_meta_xdb_end_page(void);
void causal_meta_on_xdb_close(void);

#ifdef __cplusplus
}
#endif
