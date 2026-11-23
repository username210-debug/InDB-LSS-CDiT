/*
 * 本文件实现 RocksDB 侧元数据运行时，包括 Flush 事件捕获、页面数据收集、异步维护队列、窗口状态更新、持久化和查询。
 */

#include "causal_meta.h"

#ifndef CAUSAL_META_NO_ROCKSDB
#include "rocksdb/listener.h"
#include "rocksdb/options.h"
#include "rocksdb/table_properties.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

static double elapsed_seconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double>(end - begin).count();
}

static std::string trim(const std::string& s) {
    std::size_t l = 0;
    std::size_t r = s.size();
    while (l < r && std::isspace(static_cast<unsigned char>(s[l]))) ++l;
    while (r > l && std::isspace(static_cast<unsigned char>(s[r - 1]))) --r;
    return s.substr(l, r - l);
}

static std::vector<std::string> split_simple(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

static std::string join_strings(const std::vector<std::string>& xs, char delim) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (i) oss << delim;
        oss << xs[i];
    }
    return oss.str();
}

static std::string graph_to_wire(const CDiTResult& r) {
    std::ostringstream oss;
    bool first_edge = true;
    for (const auto& kv : r.lags_by_edge) {
        if (kv.second.empty()) continue;
        if (!first_edge) oss << ';';
        first_edge = false;
        oss << kv.first.first << '>' << kv.first.second << ':';
        std::vector<int> lags = kv.second;
        std::sort(lags.begin(), lags.end());
        lags.erase(std::unique(lags.begin(), lags.end()), lags.end());
        for (std::size_t i = 0; i < lags.size(); ++i) {
            if (i) oss << ',';
            oss << lags[i];
        }
    }
    return oss.str();
}

static CDiTResult graph_from_wire(const std::string& s) {
    CDiTResult r;
    for (const std::string& edge0 : split_simple(s, ';')) {
        const std::string edge = trim(edge0);
        if (edge.empty()) continue;
        const std::size_t gt = edge.find('>');
        const std::size_t colon = edge.find(':');
        if (gt == std::string::npos || colon == std::string::npos || gt >= colon) continue;
        const int p = std::atoi(edge.substr(0, gt).c_str());
        const int q = std::atoi(edge.substr(gt + 1, colon - gt - 1).c_str());
        std::vector<int> lags;
        for (const std::string& lag0 : split_simple(edge.substr(colon + 1), ',')) {
            const std::string lag = trim(lag0);
            if (lag.empty()) continue;
            const int v = std::atoi(lag.c_str());
            if (v > 0) lags.push_back(v);
        }
        std::sort(lags.begin(), lags.end());
        lags.erase(std::unique(lags.begin(), lags.end()), lags.end());
        if (!lags.empty() && p > 0 && q > 0) {
            r.lags_by_edge[{p, q}] = std::move(lags);
        }
    }
    return r;
}

static bool parse_values_fixed(const std::string& s,
                               int expected,
                               double* out,
                               bool* fault) {
    if (expected <= 0 || out == nullptr || fault == nullptr) return false;
    const char* p = s.c_str();
    const char* end = p + s.size();
    bool all_zero = true;
    for (int i = 0; i < expected; ++i) {
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p >= end) return false;
        char* q = nullptr;
        const double v = std::strtod(p, &q);
        if (q == p) return false;
        out[i] = v;
        if (std::fabs(v) > 1e-12) all_zero = false;
        p = q;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (i + 1 < expected) {
            if (p >= end || *p != ',') return false;
            ++p;
        }
    }
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p != end) return false;
    *fault = all_zero;
    return true;
}

static std::string safe_dataset_filename(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    bool changed = false;
    for (unsigned char c : id) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
            changed = true;
        }
    }
    if (out.empty()) out = "dataset";
    if (changed) {
        // FNV-1a 后缀防止不同 id 经替换后发生文件名碰撞。
        std::uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : id) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        std::ostringstream oss;
        oss << out << '_' << std::hex << h;
        out = oss.str();
    }
    return out;
}

struct TimestampRoute {
    int raw_pos = 0;
    int sample_pos = 0;  // 0 表示该时间戳不参与当前采样步长下的因果推断。
};

// 注册阶段一次性构造、之后只读。collector 可以无锁读取该索引，避免 Flush 路径
// 每行再进入 Runtime 的全局状态锁。
struct DatasetIndex {
    int slot = -1;
    std::uint64_t generation = 0;
    int variable_count = 0;
    std::vector<CausalMetaTimestampInfo> ordered_timestamps;
    std::unordered_map<std::string_view, TimestampRoute> route_by_key;
    std::vector<int> sample_pos_to_raw_pos;  // 1-based；0 号元素为哨兵。
};

struct BufferWindow {
    int window_id = 0;
    int present_count = 0;
    int fault_count = 0;
    std::vector<std::vector<double>> data;  // 固定 T×d；窗口完成后可直接交给 CDiT，不再二次复制。
    std::vector<unsigned char> present;
    std::vector<unsigned char> fault;

    // 增量记录“本页开始触达本窗口时已有多少行”，避免每行额外维护 page->window map。
    std::uint64_t last_page_id = 0;
    int initial_count_for_page = 0;
};

struct MetaEntry {
    int window_id = 0;
    int start_pos = 0;
    int end_pos = 0;
    std::string start_key;
    std::string end_key;
    std::string start_label;
    std::string end_label;
    CDiTResult graph;
};

struct DatasetState {
    CausalMetaDatasetConfig cfg;  // ordered_timestamps 在这里保持为空，避免与索引再复制一份大向量。
    std::shared_ptr<DatasetIndex> index;

    // C 始终按 window_id 有序；插入/替换 O(log n)，不做每页全量 sort。
    std::map<int, MetaEntry> entries;

    // B 只按当前页实际触达的窗口局部维护；窗口内部使用 O(1) 定位的定长数组。
    std::unordered_map<int, BufferWindow> buffers;

    long long inferred_windows = 0;
    long long original_complete_windows = 0;
};

struct CaptureRegistry {
    std::uint64_t generation = 0;
    // 数据集数通常很小，透明比较的 map 允许直接用 string_view 查找，不分配临时 dataset_id 字符串。
    std::map<std::string, std::shared_ptr<const DatasetIndex>, std::less<>> datasets;
};

struct CapturedRow {
    int slot = -1;
    int sample_pos = 0;
    std::uint32_t value_offset = 0;  // 指向 PageCapture::numeric_values。
    bool fault = false;
};

struct PageCapture {
    std::uint64_t page_id = 0;
    std::uint64_t generation = 0;
    bool has_registered = false;
    std::vector<CapturedRow> rows;
    std::vector<double> numeric_values;

    // 只有真正要求页面日志时才填充；性能实验关闭日志时完全不维护这些位置列表。
    std::unordered_map<int, std::vector<int>> raw_positions_by_slot;
    std::string first_error;
    double capture_seconds = 0.0;
};

struct PageDatasetFlags {
    bool touched = false;
    bool complementary = false;
    bool overlapped = false;
    bool adjacent = false;
    int initial_last_entry_window = -1;
    std::vector<int> raw_positions;
};

static bool capture_one_kv(const CaptureRegistry& registry,
                           bool page_logging,
                           const char* key_data,
                           std::size_t key_size,
                           const char* value_data,
                           std::size_t value_size,
                           PageCapture* page,
                           std::string* value_scratch,
                           int* last_slot,
                           int* last_raw_pos) {
    if (page == nullptr || value_scratch == nullptr || last_slot == nullptr || last_raw_pos == nullptr) {
        return false;
    }
    const void* comma0 = std::memchr(key_data, ',', key_size);
    if (comma0 == nullptr) return false;
    const char* comma = static_cast<const char*>(comma0);
    if (comma == key_data || comma + 1 >= key_data + key_size) return false;

    const std::string_view dataset_id(key_data, static_cast<std::size_t>(comma - key_data));
    const std::string_view ts(comma + 1, static_cast<std::size_t>((key_data + key_size) - (comma + 1)));
    auto dsit = registry.datasets.find(dataset_id);
    if (dsit == registry.datasets.end()) return false;
    const DatasetIndex& index = *dsit->second;
    auto rit = index.route_by_key.find(ts);
    if (rit == index.route_by_key.end()) return false;

    page->has_registered = true;
    const TimestampRoute route = rit->second;

    // RocksDB 同一 user key 的内部版本在 Flush 表中相邻；对本模块而言 slot+raw_pos 唯一标识 user key。
    // 因而无需为去重再把每个 key 复制进 unordered_set。
    if (*last_slot == index.slot && *last_raw_pos == route.raw_pos) return true;
    *last_slot = index.slot;
    *last_raw_pos = route.raw_pos;

    if (page_logging) {
        page->raw_positions_by_slot[index.slot].push_back(route.raw_pos);
    }
    if (route.sample_pos == 0) return true;

    const std::size_t old_size = page->numeric_values.size();
    const std::size_t d = static_cast<std::size_t>(index.variable_count);
    if (old_size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        d > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - old_size) {
        if (page->first_error.empty()) page->first_error = "causal_meta: 单页采样数值过多，value_offset 超出 uint32_t";
        return false;
    }

    page->numeric_values.resize(old_size + d);
    value_scratch->assign(value_data, value_size);
    bool fault = false;
    if (!parse_values_fixed(*value_scratch, index.variable_count,
                            page->numeric_values.data() + old_size, &fault)) {
        page->numeric_values.resize(old_size);
        if (page->first_error.empty()) {
            page->first_error = "causal_meta: 无法解析数据集 " + std::string(dataset_id) +
                                " 时间戳 " + std::string(ts) + " 的 value";
        }
        return false;
    }

    CapturedRow row;
    row.slot = index.slot;
    row.sample_pos = route.sample_pos;
    row.value_offset = static_cast<std::uint32_t>(old_size);
    row.fault = fault;
    page->rows.push_back(row);
    return true;
}

class Runtime {
public:
    Runtime() = default;
    ~Runtime() {
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            stop_ = true;
        }
        queue_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    void ensure_worker() {
        std::lock_guard<std::mutex> lk(queue_mu_);
        if (!worker_.joinable()) {
            stop_ = false;
            worker_ = std::thread([this] { worker_loop(); });
        }
    }

    void enqueue(PageCapture page) {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        ensure_worker();
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            queue_.push_back(std::move(page));
        }
        queue_cv_.notify_one();
    }

    void wait_idle() {
        std::unique_lock<std::mutex> lk(queue_mu_);
        idle_cv_.wait(lk, [this] {
            return queue_.empty() && in_flight_ == 0 && pending_pages_.empty();
        });
    }

    void stage_pending(std::uint64_t token, PageCapture page) {
        std::lock_guard<std::mutex> lk(queue_mu_);
        pending_pages_[token] = std::move(page);
    }

    void commit_pending(std::uint64_t token, bool success) {
        PageCapture page;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            auto it = pending_pages_.find(token);
            if (it != pending_pages_.end()) {
                if (success) {
                    page = std::move(it->second);
                    have = true;
                }
                pending_pages_.erase(it);
            }
        }
        idle_cv_.notify_all();
        if (have) enqueue(std::move(page));
    }

    void reset() {
        wait_idle();
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            datasets_.clear();
            states_by_slot_.clear();
            counters_ = CausalMetaCounters{};
            finish_capture_seconds_.store(0.0, std::memory_order_relaxed);
            dirty_ = false;
            metadata_dir_ = "meta_file";
            page_logging_ = false;
            page_logging_fast_.store(false, std::memory_order_release);
            sink_ = nullptr;
            sink_userdata_ = nullptr;
            last_error_.clear();
            ++generation_;
            auto empty = std::make_shared<CaptureRegistry>();
            empty->generation = generation_;
            std::shared_ptr<const CaptureRegistry> published = empty;
            std::atomic_store_explicit(&capture_registry_, published, std::memory_order_release);
        }
        enabled_.store(true, std::memory_order_relaxed);
    }

    bool register_dataset(const CausalMetaDatasetConfig& cfg, std::string* error) {
        if (cfg.dataset_id.empty()) return set_error(error, "dataset_id 不能为空");
        if (cfg.variable_names.empty()) return set_error(error, "variable_names 不能为空");
        if (cfg.ordered_timestamps.empty()) return set_error(error, "ordered_timestamps 不能为空");
        if (cfg.sampling_step <= 0) return set_error(error, "sampling_step 必须 > 0");
        if (cfg.window_length <= 0) return set_error(error, "window_length 必须 > 0");
        if (cfg.tau <= 0) return set_error(error, "tau 必须 > 0");
        if (cfg.window_length < 2 * cfg.tau) {
            return set_error(error, "window_length 必须至少为 2*tau，否则 CDiT 没有有效样本");
        }

        // 先构造只读索引。时间戳正文只保留这一份；hash 表的 key 使用 string_view 指向它，
        // 避免原实现为 raw/sample/label 三个哈希表重复保存同一批字符串。
        auto index = std::make_shared<DatasetIndex>();
        index->variable_count = static_cast<int>(cfg.variable_names.size());
        index->ordered_timestamps = cfg.ordered_timestamps;
        index->sample_pos_to_raw_pos.push_back(0);
        index->route_by_key.reserve(cfg.ordered_timestamps.size());

        int sample_pos = 0;
        for (std::size_t i = 0; i < index->ordered_timestamps.size(); ++i) {
            const CausalMetaTimestampInfo& ts = index->ordered_timestamps[i];
            if (ts.key.empty()) return set_error(error, "ordered_timestamps 中存在空 key");
            const int raw_pos = static_cast<int>(i) + 1;
            TimestampRoute route;
            route.raw_pos = raw_pos;
            if ((static_cast<int>(i) % cfg.sampling_step) == 0) {
                route.sample_pos = ++sample_pos;
                index->sample_pos_to_raw_pos.push_back(raw_pos);
            }
            const auto inserted = index->route_by_key.emplace(std::string_view(ts.key), route);
            if (!inserted.second) {
                return set_error(error, "ordered_timestamps 中存在重复时间戳: " + ts.key);
            }
        }

        DatasetState ds;
        // 只复制运行参数，不把 ordered_timestamps 再复制到 cfg。
        ds.cfg.dataset_id = cfg.dataset_id;
        ds.cfg.display_name = cfg.display_name;
        ds.cfg.variable_names = cfg.variable_names;
        ds.cfg.sampling_step = cfg.sampling_step;
        ds.cfg.window_length = cfg.window_length;
        ds.cfg.tau = cfg.tau;
        ds.cfg.alpha = cfg.alpha;
        ds.cfg.infer_self = cfg.infer_self;
        ds.cfg.use_covariance_cache_cdit = cfg.use_covariance_cache_cdit;
        ds.cfg.source_columns = cfg.source_columns;
        ds.cfg.target_columns = cfg.target_columns;
        ds.index = index;

        std::lock_guard<std::mutex> lk(state_mu_);
        if (datasets_.find(cfg.dataset_id) != datasets_.end()) {
            return set_error_locked(error, "重复注册数据集: " + cfg.dataset_id);
        }
        index->slot = static_cast<int>(states_by_slot_.size());
        index->generation = generation_;
        auto inserted = datasets_.emplace(cfg.dataset_id, std::move(ds));
        DatasetState* state = &inserted.first->second;
        states_by_slot_.push_back(state);

        auto old_registry = std::atomic_load_explicit(&capture_registry_, std::memory_order_acquire);
        auto next_registry = std::make_shared<CaptureRegistry>();
        next_registry->generation = generation_;
        if (old_registry && old_registry->generation == generation_) {
            next_registry->datasets = old_registry->datasets;
        }
        next_registry->datasets[cfg.dataset_id] = index;
        std::shared_ptr<const CaptureRegistry> published = next_registry;
        std::atomic_store_explicit(&capture_registry_, published, std::memory_order_release);
        return true;
    }

    bool has_dataset(const std::string& id) const {
        std::lock_guard<std::mutex> lk(state_mu_);
        return datasets_.find(id) != datasets_.end();
    }

    std::vector<std::string> dataset_ids() const {
        std::lock_guard<std::mutex> lk(state_mu_);
        std::vector<std::string> out;
        out.reserve(datasets_.size());
        for (const auto& kv : datasets_) out.push_back(kv.first);
        return out;
    }

    void set_enabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    void set_metadata_dir(std::string dir) {
        std::lock_guard<std::mutex> lk(state_mu_);
        metadata_dir_ = std::move(dir);
    }

    std::string metadata_dir() const {
        std::lock_guard<std::mutex> lk(state_mu_);
        return metadata_dir_;
    }

    void set_page_logging(bool v) {
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            page_logging_ = v;
        }
        page_logging_fast_.store(v, std::memory_order_release);
    }

    bool page_logging() const {
        return page_logging_fast_.load(std::memory_order_acquire);
    }

    std::shared_ptr<const CaptureRegistry> capture_registry_snapshot() const {
        return std::atomic_load_explicit(&capture_registry_, std::memory_order_acquire);
    }

    void set_sink(void (*sink)(const char*, void*), void* userdata) {
        std::lock_guard<std::mutex> lk(state_mu_);
        sink_ = sink;
        sink_userdata_ = userdata;
    }

    void clear_sink() {
        std::lock_guard<std::mutex> lk(state_mu_);
        sink_ = nullptr;
        sink_userdata_ = nullptr;
    }

    CausalMetaCounters counters() const {
        std::lock_guard<std::mutex> lk(state_mu_);
        CausalMetaCounters out = counters_;
        long long entries = 0;
        long long inferred = 0;
        long long original = 0;
        for (const auto& kv : datasets_) {
            entries += static_cast<long long>(kv.second.entries.size());
            inferred += kv.second.inferred_windows;
            original += kv.second.original_complete_windows;
        }
        out.final_metadata_entries = entries;
        out.inferred_windows = inferred;
        out.original_complete_windows = original;
        out.flush_capture_seconds += finish_capture_seconds_.load(std::memory_order_relaxed);
        // metadata_seconds 统一在读取时由三个互斥定义项组成，避免异步路径重复累计。
        out.metadata_seconds = out.flush_capture_seconds + out.worker_seconds + out.persistence_seconds;
        return out;
    }

    std::string format_metadata(const std::string& id) const {
        std::lock_guard<std::mutex> lk(state_mu_);
        auto it = datasets_.find(id);
        if (it == datasets_.end()) return std::string();
        return format_metadata_locked(it->second);
    }

    std::string format_buffers(const std::string& id) const {
        std::lock_guard<std::mutex> lk(state_mu_);
        auto it = datasets_.find(id);
        if (it == datasets_.end()) return std::string();
        return format_buffers_locked(it->second);
    }

    bool persist(std::string* error) {
        wait_idle();
        const Clock::time_point t0 = Clock::now();
        bool ok = true;
        std::string err;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (!dirty_) return true;
            try {
                const std::filesystem::path dir(metadata_dir_);
                std::filesystem::create_directories(dir);
                for (const auto& kv : datasets_) {
                    if (!persist_dataset_locked(dir, kv.second, &err)) {
                        ok = false;
                        break;
                    }
                }
                if (ok) dirty_ = false;
            } catch (const std::exception& e) {
                ok = false;
                err = e.what();
            } catch (...) {
                ok = false;
                err = "未知持久化异常";
            }
        }
        const double sec = elapsed_seconds(t0, Clock::now());
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            counters_.persistence_seconds += sec;
            if (!ok) last_error_ = err;
        }
        if (!ok && error) *error = err;
        return ok;
    }

    // Finish/stage 的极小开销不能去拿 state_mu_，否则会把 Flush 线程与正在执行 CDiT 的 worker 串行化。
    // C++20 的 atomic<double>::fetch_add 让这个计时统计保持无锁。
    void add_finish_capture_seconds(double sec) {
        finish_capture_seconds_.fetch_add(sec, std::memory_order_relaxed);
    }

    void emit_external(const std::string& text) const {
        void (*sink)(const char*, void*) = nullptr;
        void* userdata = nullptr;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            sink = sink_;
            userdata = sink_userdata_;
        }
        if (sink) sink(text.c_str(), userdata);
    }

    void set_last_error(const std::string& e) {
        std::lock_guard<std::mutex> lk(state_mu_);
        last_error_ = e;
    }

    // 用于查询时取得运行时的时间戳 -> sample_pos 映射；无则返回 false。
    bool query_boundary_positions(const std::string& dataset_id,
                                  const std::string& start_key,
                                  const std::string& end_key,
                                  int* start_pos,
                                  int* end_pos,
                                  std::string* error) const {
        std::lock_guard<std::mutex> lk(state_mu_);
        auto it = datasets_.find(dataset_id);
        if (it == datasets_.end()) return false;
        const DatasetState& ds = it->second;
        const DatasetIndex& index = *ds.index;
        const int max_pos = static_cast<int>(index.sample_pos_to_raw_pos.size()) - 1;
        *start_pos = 1;
        *end_pos = max_pos;
        if (!start_key.empty()) {
            auto p = index.route_by_key.find(std::string_view(start_key));
            if (p == index.route_by_key.end() || p->second.sample_pos == 0) {
                if (error) *error = "查询起点不是已注册的采样时间戳: " + start_key;
                return false;
            }
            *start_pos = p->second.sample_pos;
        }
        if (!end_key.empty()) {
            auto p = index.route_by_key.find(std::string_view(end_key));
            if (p == index.route_by_key.end() || p->second.sample_pos == 0) {
                if (error) *error = "查询终点不是已注册的采样时间戳: " + end_key;
                return false;
            }
            *end_pos = p->second.sample_pos;
        }
        return true;
    }

    void submit_page_for_test(std::uint64_t page_id,
                              const std::vector<std::pair<std::string, std::string>>& key_values) {
        if (!enabled()) return;
        auto registry = capture_registry_snapshot();
        if (!registry) return;
        PageCapture page;
        page.page_id = page_id;
        page.generation = registry->generation;
        page.rows.reserve(key_values.size());
        std::string scratch;
        int last_slot = -1;
        int last_raw_pos = -1;
        const bool logging = page_logging_fast_.load(std::memory_order_acquire);
        const Clock::time_point t0 = Clock::now();
        for (const auto& kv : key_values) {
            capture_one_kv(*registry, logging,
                           kv.first.data(), kv.first.size(),
                           kv.second.data(), kv.second.size(),
                           &page, &scratch, &last_slot, &last_raw_pos);
        }
        page.capture_seconds += elapsed_seconds(t0, Clock::now());
        enqueue(std::move(page));
    }

private:
    bool set_error(std::string* error, const std::string& msg) {
        std::lock_guard<std::mutex> lk(state_mu_);
        return set_error_locked(error, msg);
    }

    bool set_error_locked(std::string* error, const std::string& msg) {
        last_error_ = msg;
        if (error) *error = msg;
        return false;
    }

    static std::string graph_named_ordered(const CDiTResult& graph,
                                           const std::vector<std::string>& names,
                                           const std::vector<std::string>& preferred_order) {
        std::map<std::string, int> rank;
        for (std::size_t i = 0; i < preferred_order.size(); ++i) {
            rank[preferred_order[i]] = static_cast<int>(i);
        }
        auto name_of = [&](int idx) {
            if (idx >= 1 && idx <= static_cast<int>(names.size()) &&
                !names[static_cast<std::size_t>(idx - 1)].empty()) {
                return names[static_cast<std::size_t>(idx - 1)];
            }
            return std::string("X") + std::to_string(idx);
        };
        struct Item {
            int rp, rq, p, q;
            std::string ps, qs;
            std::vector<int> lags;
        };
        std::vector<Item> items;
        for (const auto& kv : graph.lags_by_edge) {
            if (kv.second.empty()) continue;
            const std::string ps = name_of(kv.first.first);
            const std::string qs = name_of(kv.first.second);
            const int rp = rank.count(ps) ? rank[ps] : 100000 + kv.first.first;
            const int rq = rank.count(qs) ? rank[qs] : 100000 + kv.first.second;
            items.push_back({rp, rq, kv.first.first, kv.first.second, ps, qs, kv.second});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.rp != b.rp) return a.rp < b.rp;
            if (a.rq != b.rq) return a.rq < b.rq;
            if (a.p != b.p) return a.p < b.p;
            return a.q < b.q;
        });
        std::ostringstream oss;
        for (const auto& item : items) {
            std::vector<int> lags = item.lags;
            std::sort(lags.begin(), lags.end());
            lags.erase(std::unique(lags.begin(), lags.end()), lags.end());
            oss << item.ps << "→" << item.qs << "滞后=";
            for (int lag : lags) oss << lag << ',';
            oss << "; ";
        }
        return oss.str();
    }

    static const CausalMetaTimestampInfo* timestamp_for_raw(const DatasetState& ds, int raw_pos) {
        if (!ds.index || raw_pos < 1 || raw_pos > static_cast<int>(ds.index->ordered_timestamps.size())) {
            return nullptr;
        }
        return &ds.index->ordered_timestamps[static_cast<std::size_t>(raw_pos - 1)];
    }

    static const CausalMetaTimestampInfo* timestamp_for_sample(const DatasetState& ds, int sample_pos) {
        if (!ds.index || sample_pos < 1 ||
            sample_pos >= static_cast<int>(ds.index->sample_pos_to_raw_pos.size())) {
            return nullptr;
        }
        return timestamp_for_raw(ds, ds.index->sample_pos_to_raw_pos[static_cast<std::size_t>(sample_pos)]);
    }

    static std::string label_for_sample(const DatasetState& ds, int pos) {
        const CausalMetaTimestampInfo* ts = timestamp_for_sample(ds, pos);
        if (!ts) return std::string();
        return ts->label.empty() ? ts->key : ts->label;
    }

    static std::string key_for_sample(const DatasetState& ds, int pos) {
        const CausalMetaTimestampInfo* ts = timestamp_for_sample(ds, pos);
        return ts ? ts->key : std::string();
    }

    static std::string label_for_raw(const DatasetState& ds, int pos) {
        const CausalMetaTimestampInfo* ts = timestamp_for_raw(ds, pos);
        if (!ts) return std::string();
        return ts->label.empty() ? ts->key : ts->label;
    }

    static std::string format_metadata_locked(const DatasetState& ds) {
        std::ostringstream oss;
        oss << (ds.cfg.display_name.empty() ? ds.cfg.dataset_id : ds.cfg.display_name)
            << "的元数据：";
        for (const auto& kv : ds.entries) {
            const MetaEntry& e = kv.second;
            oss << '[' << e.start_label << ", " << e.end_label << "],";
            oss << graph_named_ordered(e.graph, ds.cfg.variable_names, ds.cfg.variable_names);
        }
        return oss.str();
    }

    static std::string format_buffers_locked(const DatasetState& ds) {
        std::vector<int> ids;
        ids.reserve(ds.buffers.size());
        for (const auto& kv : ds.buffers) ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end());

        std::ostringstream oss;
        oss << (ds.cfg.display_name.empty() ? ds.cfg.dataset_id : ds.cfg.display_name)
            << "的暂存区：";
        for (int id : ids) {
            const BufferWindow& w = ds.buffers.at(id);
            if (w.present_count == 0) continue;
            int first_off = -1;
            int last_off = -1;
            for (int off = 0; off < static_cast<int>(w.present.size()); ++off) {
                if (!w.present[static_cast<std::size_t>(off)]) continue;
                if (first_off < 0) first_off = off;
                last_off = off;
            }
            if (first_off < 0) continue;
            const int start_pos = id * ds.cfg.window_length + first_off + 1;
            const int end_pos = id * ds.cfg.window_length + last_off + 1;
            const bool complete = w.present_count >= ds.cfg.window_length;
            const bool failed = w.fault_count > 0;
            oss << label_for_sample(ds, start_pos) << "到" << label_for_sample(ds, end_pos)
                << '(';
            if (!complete && !failed) oss << "不完整不失败";
            else if (!complete && failed) oss << "不完整失败";
            else if (complete && failed) oss << "完整失败";
            else oss << "完整不失败";
            oss << "); ";
        }
        return oss.str();
    }

    static std::string build_page_prefix_locked(
            const std::vector<PageDatasetFlags>& flags,
            const std::vector<DatasetState*>& states_by_slot) {
        std::ostringstream oss;
        oss << "[ ";
        bool first_span = true;
        for (std::size_t slot = 0; slot < flags.size() && slot < states_by_slot.size(); ++slot) {
            const PageDatasetFlags& flag = flags[slot];
            if (!flag.touched || flag.raw_positions.empty() || states_by_slot[slot] == nullptr) continue;
            const DatasetState& ds = *states_by_slot[slot];
            std::vector<int> pos = flag.raw_positions;
            std::sort(pos.begin(), pos.end());
            pos.erase(std::unique(pos.begin(), pos.end()), pos.end());
            std::size_t i = 0;
            while (i < pos.size()) {
                std::size_t j = i;
                while (j + 1 < pos.size() && pos[j + 1] == pos[j] + 1) ++j;
                if (!first_span) oss << " | ";
                first_span = false;
                const std::string& display = ds.cfg.display_name.empty() ? ds.cfg.dataset_id : ds.cfg.display_name;
                oss << display << "的" << label_for_raw(ds, pos[i])
                    << "到" << label_for_raw(ds, pos[j]);
                i = j + 1;
            }
        }
        oss << " ]";

        bool complementary = false;
        bool overlapped = false;
        bool adjacent = false;
        for (const auto& flag : flags) {
            if (!flag.touched) continue;
            complementary = complementary || flag.complementary;
            overlapped = overlapped || flag.overlapped;
            adjacent = adjacent || flag.adjacent;
        }
        if (complementary) oss << " 互补页面";
        if (overlapped) oss << " 重叠页面";
        if (adjacent) oss << " 相邻页面";
        return oss.str();
    }

    static bool write_atomic_file(const std::filesystem::path& path,
                                  const std::string& contents,
                                  std::string* error) {
        const std::filesystem::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                if (error) *error = "无法创建文件: " + tmp.string();
                return false;
            }
            out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            out.flush();
            if (!out) {
                if (error) *error = "写文件失败: " + tmp.string();
                return false;
            }
        }
        // Ubuntu/Linux 上 rename(2) 对同一文件系统中的现有目标是原子替换；
        // 先直接 rename，避免“先删旧文件再改名”带来的崩溃空窗。
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            // 兼顾某些不允许覆盖目标文件的平台：仅在原子替换失败后退化为 remove + rename。
            const std::string first_error = ec.message();
            ec.clear();
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(tmp, path, ec);
            if (ec) {
                if (error) {
                    *error = "重命名临时文件失败: " + first_error + "; fallback: " + ec.message();
                }
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }
        return true;
    }

    static bool persist_dataset_locked(const std::filesystem::path& dir,
                                       const DatasetState& ds,
                                       std::string* error) {
        const std::string base = safe_dataset_filename(ds.cfg.dataset_id);

        std::ostringstream c;
        c << "#CAUSAL_META_C_V2\n";
        c << "#dataset\t" << ds.cfg.dataset_id << '\n';
        c << "#display\t" << ds.cfg.display_name << '\n';
        c << "#variables\t" << join_strings(ds.cfg.variable_names, ',') << '\n';
        c << "#sampling_step\t" << ds.cfg.sampling_step << '\n';
        c << "#window_length\t" << ds.cfg.window_length << '\n';
        c << "#tau\t" << ds.cfg.tau << '\n';
        c << "#alpha\t" << std::setprecision(17) << ds.cfg.alpha << '\n';
        c << "#use_covariance_cache_cdit\t" << (ds.cfg.use_covariance_cache_cdit ? 1 : 0) << '\n';
        c << "window_id\tstart_pos\tend_pos\tstart_key\tend_key\tstart_label\tend_label\tgraph\n";
        for (const auto& kv : ds.entries) {
            const MetaEntry& e = kv.second;
            c << e.window_id << '\t' << e.start_pos << '\t' << e.end_pos << '\t'
              << e.start_key << '\t' << e.end_key << '\t'
              << e.start_label << '\t' << e.end_label << '\t'
              << graph_to_wire(e.graph) << '\n';
        }
        if (!write_atomic_file(dir / (base + "_C.tsv"), c.str(), error)) return false;

        std::ostringstream b;
        b << "#CAUSAL_META_B_V2\n";
        b << "#dataset\t" << ds.cfg.dataset_id << '\n';
        b << "#variables\t" << join_strings(ds.cfg.variable_names, ',') << '\n';
        b << "window_id\tsample_pos\ttimestamp_key\tlabel\tfault\tvalues\n";
        std::vector<int> bids;
        bids.reserve(ds.buffers.size());
        for (const auto& kv : ds.buffers) bids.push_back(kv.first);
        std::sort(bids.begin(), bids.end());
        for (int wid : bids) {
            const BufferWindow& w = ds.buffers.at(wid);
            for (int off = 0; off < static_cast<int>(w.present.size()); ++off) {
                if (!w.present[static_cast<std::size_t>(off)]) continue;
                const int sample_pos = wid * ds.cfg.window_length + off + 1;
                b << wid << '\t' << sample_pos << '\t'
                  << key_for_sample(ds, sample_pos) << '\t'
                  << label_for_sample(ds, sample_pos) << '\t'
                  << (w.fault[static_cast<std::size_t>(off)] ? 1 : 0) << '\t';
                const std::vector<double>& values = w.data[static_cast<std::size_t>(off)];
                for (std::size_t i = 0; i < values.size(); ++i) {
                    if (i) b << ',';
                    b << std::setprecision(17) << values[i];
                }
                b << '\n';
            }
        }
        if (!write_atomic_file(dir / (base + "_B.tsv"), b.str(), error)) return false;

        // 单独保存时间戳顺序索引，使 C/B 文件仍保持紧凑；未来查询无需解析时间戳格式。
        std::ostringstream t;
        t << "#CAUSAL_META_T_V1\n";
        t << "#dataset\t" << ds.cfg.dataset_id << '\n';
        t << "sample_pos\ttimestamp_key\tlabel\n";
        const int sample_count = ds.index ? static_cast<int>(ds.index->sample_pos_to_raw_pos.size()) - 1 : 0;
        for (int pos = 1; pos <= sample_count; ++pos) {
            t << pos << '\t' << key_for_sample(ds, pos) << '\t'
              << label_for_sample(ds, pos) << '\n';
        }
        if (!write_atomic_file(dir / (base + "_T.tsv"), t.str(), error)) return false;

        return true;
    }

    void worker_loop() {
        for (;;) {
            PageCapture page;
            {
                std::unique_lock<std::mutex> lk(queue_mu_);
                queue_cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) break;
                page = std::move(queue_.front());
                queue_.pop_front();
                ++in_flight_;
            }

            const double capture_sec = page.capture_seconds;
            const Clock::time_point t0 = Clock::now();
            process_page(std::move(page));
            const double worker_sec = elapsed_seconds(t0, Clock::now());
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                counters_.worker_seconds += worker_sec;
                counters_.flush_capture_seconds += capture_sec;
            }

            {
                std::lock_guard<std::mutex> lk(queue_mu_);
                --in_flight_;
            }
            idle_cv_.notify_all();
        }
    }

    void process_page(PageCapture page) {
        if (!enabled_.load(std::memory_order_relaxed)) return;

        std::vector<std::string> log_lines;
        try {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (page.generation != generation_) return;
            if (!page.first_error.empty()) last_error_ = page.first_error;
            if (!page.has_registered) return;

            std::vector<PageDatasetFlags> flags(states_by_slot_.size());
            const bool need_log = page_logging_ && sink_ != nullptr;

            // 只有开启页面日志时 collector 才保存 raw_positions；这里直接 move，避免再复制一遍。
            if (need_log) {
                for (auto& kv : page.raw_positions_by_slot) {
                    if (kv.first < 0 || kv.first >= static_cast<int>(flags.size())) continue;
                    PageDatasetFlags& f = flags[static_cast<std::size_t>(kv.first)];
                    f.touched = true;
                    f.raw_positions = std::move(kv.second);
                    DatasetState* ds = states_by_slot_[static_cast<std::size_t>(kv.first)];
                    if (ds != nullptr) {
                        f.initial_last_entry_window = ds->entries.empty() ? -1 : ds->entries.rbegin()->first;
                    }
                }
            }

            for (const CapturedRow& row : page.rows) {
                if (row.slot < 0 || row.slot >= static_cast<int>(states_by_slot_.size())) continue;
                DatasetState* dsp = states_by_slot_[static_cast<std::size_t>(row.slot)];
                if (dsp == nullptr) continue;
                DatasetState& ds = *dsp;
                if (!ds.index || ds.index->generation != page.generation) continue;

                PageDatasetFlags& flag = flags[static_cast<std::size_t>(row.slot)];
                if (!flag.touched) {
                    flag.touched = true;
                    flag.initial_last_entry_window = ds.entries.empty() ? -1 : ds.entries.rbegin()->first;
                }

                const int T = ds.cfg.window_length;
                const int d = static_cast<int>(ds.cfg.variable_names.size());
                const int window_id = (row.sample_pos - 1) / T;
                const int local = (row.sample_pos - 1) % T;

                auto bit = ds.buffers.find(window_id);
                if (bit == ds.buffers.end()) {
                    BufferWindow fresh;
                    fresh.window_id = window_id;
                    fresh.data.assign(static_cast<std::size_t>(T),
                                      std::vector<double>(static_cast<std::size_t>(d), 0.0));
                    fresh.present.assign(static_cast<std::size_t>(T), 0);
                    fresh.fault.assign(static_cast<std::size_t>(T), 0);
                    bit = ds.buffers.emplace(window_id, std::move(fresh)).first;
                }
                BufferWindow& w = bit->second;
                if (w.last_page_id != page.page_id) {
                    w.last_page_id = page.page_id;
                    w.initial_count_for_page = w.present_count;
                }
                const int initial_count_for_page = w.initial_count_for_page;

                const std::size_t local_u = static_cast<std::size_t>(local);
                const bool had_old = w.present[local_u] != 0;
                if (had_old) {
                    flag.overlapped = true;
                    if (w.fault[local_u]) --w.fault_count;
                }

                const std::size_t off = static_cast<std::size_t>(row.value_offset);
                if (off + static_cast<std::size_t>(d) > page.numeric_values.size()) {
                    last_error_ = "causal_meta: PageCapture numeric_values 越界";
                    if (had_old && w.fault[local_u]) ++w.fault_count;
                    continue;
                }
                std::copy_n(page.numeric_values.begin() + static_cast<std::ptrdiff_t>(off),
                            d, w.data[local_u].begin());
                if (!had_old) {
                    w.present[local_u] = 1;
                    ++w.present_count;
                }
                w.fault[local_u] = row.fault ? 1 : 0;
                if (row.fault) ++w.fault_count;

                const bool now_structurally_complete = w.present_count == T;
                if (initial_count_for_page > 0 &&
                    initial_count_for_page < T &&
                    now_structurally_complete) {
                    flag.complementary = true;
                }

                if (now_structurally_complete && w.fault_count == 0) {
                    const int start_pos = window_id * T + 1;
                    const int end_pos = start_pos + T - 1;
                    if (initial_count_for_page == 0) ++ds.original_complete_windows;

                    const std::vector<int>* src = ds.cfg.source_columns.empty() ? nullptr : &ds.cfg.source_columns;
                    const std::vector<int>* dst = ds.cfg.target_columns.empty() ? nullptr : &ds.cfg.target_columns;
                    // w.data 已经是完整 T×d，直接推断，不再像旧实现那样逐行从 std::map 拷贝出第二份 data。
                    CDiTResult graph = ds.cfg.use_covariance_cache_cdit
                            ? run_cdit_covmeta(w.data, ds.cfg.tau, ds.cfg.alpha, ds.cfg.infer_self, src, dst)
                            : run_cdit_original(w.data, ds.cfg.tau, ds.cfg.alpha, ds.cfg.infer_self, src, dst);

                    const bool adjacent = flag.initial_last_entry_window >= 0 &&
                                          window_id == flag.initial_last_entry_window + 1;
                    flag.adjacent = flag.adjacent || adjacent;

                    MetaEntry e;
                    e.window_id = window_id;
                    e.start_pos = start_pos;
                    e.end_pos = end_pos;
                    e.start_key = key_for_sample(ds, start_pos);
                    e.end_key = key_for_sample(ds, end_pos);
                    e.start_label = label_for_sample(ds, start_pos);
                    e.end_label = label_for_sample(ds, end_pos);
                    e.graph = std::move(graph);
                    ds.entries[window_id] = std::move(e);
                    ds.buffers.erase(bit);
                    ++ds.inferred_windows;
                }
            }

            ++counters_.total_pages;
            bool page_comp = false;
            bool page_overlap = false;
            for (const PageDatasetFlags& flag : flags) {
                if (!flag.touched) continue;
                page_comp = page_comp || flag.complementary;
                page_overlap = page_overlap || flag.overlapped;
            }
            if (page_comp) ++counters_.complementary_pages;
            if (page_overlap) ++counters_.overlapped_pages;
            dirty_ = true;

            // 性能关键：日志关闭时 collector 连 raw_positions 都不保存，这里更不会组织 C/B 字符串。
            if (need_log) {
                log_lines.push_back(build_page_prefix_locked(flags, states_by_slot_));
                for (std::size_t slot = 0; slot < flags.size(); ++slot) {
                    if (!flags[slot].touched || states_by_slot_[slot] == nullptr) continue;
                    log_lines.push_back(format_metadata_locked(*states_by_slot_[slot]));
                    log_lines.push_back(format_buffers_locked(*states_by_slot_[slot]));
                }
            }
        } catch (const std::exception& e) {
            set_last_error(std::string("causal_meta 页面处理异常: ") + e.what());
            log_lines.push_back(std::string("causal_meta 页面处理异常: ") + e.what());
        } catch (...) {
            set_last_error("causal_meta 页面处理未知异常");
            log_lines.push_back("causal_meta 页面处理未知异常");
        }

        if (!log_lines.empty()) {
            for (const auto& line : log_lines) emit_external(line);
        }
    }

private:
    std::atomic<bool> enabled_{true};
    std::atomic<bool> page_logging_fast_{false};
    std::atomic<double> finish_capture_seconds_{0.0};

    mutable std::mutex state_mu_;
    std::map<std::string, DatasetState> datasets_;
    std::vector<DatasetState*> states_by_slot_;
    CausalMetaCounters counters_;
    std::string metadata_dir_ = "meta_file";
    bool page_logging_ = false;
    bool dirty_ = false;
    void (*sink_)(const char*, void*) = nullptr;
    void* sink_userdata_ = nullptr;
    std::string last_error_;
    std::uint64_t generation_ = 1;
    std::shared_ptr<const CaptureRegistry> capture_registry_ = [] {
        auto r = std::make_shared<CaptureRegistry>();
        r->generation = 1;
        return std::shared_ptr<const CaptureRegistry>(r);
    }();

    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::condition_variable idle_cv_;
    std::deque<PageCapture> queue_;
    std::unordered_map<std::uint64_t, PageCapture> pending_pages_;
    std::thread worker_;
    std::size_t in_flight_ = 0;
    bool stop_ = false;
};

Runtime G;

#ifndef CAUSAL_META_NO_ROCKSDB

// 一次 SST 构建发生在一个后台线程上；OnTableFileCreationStarted -> collector factory
// -> collector callbacks -> OnTableFileCreated 都在这条实际建表线程上。
thread_local bool tls_causal_flush = false;
thread_local std::uint64_t tls_causal_token = 0;
std::atomic<std::uint64_t> g_next_token{1};

class CausalMetaTableCollector final : public rocksdb::TablePropertiesCollector {
public:
    CausalMetaTableCollector(std::uint64_t token,
                             std::shared_ptr<const CaptureRegistry> registry,
                             bool page_logging)
        : token_(token), registry_(std::move(registry)), page_logging_(page_logging) {
        page_.page_id = token;
        page_.generation = registry_ ? registry_->generation : 0;
        // 仅预留紧凑的采样行/数值缓冲；不再为整页复制 key/value 字符串。
        page_.rows.reserve(1024);
        page_.numeric_values.reserve(4096);
    }

    rocksdb::Status AddUserKey(const rocksdb::Slice& key,
                               const rocksdb::Slice& value,
                               rocksdb::EntryType type,
                               rocksdb::SequenceNumber,
                               std::uint64_t) override {
        if (!G.enabled() || !registry_) return rocksdb::Status::OK();
        // 本实验只把 Put/TimedPut 作为时间序列样本；其它内部记录类型全部忽略。
        if (type != rocksdb::kEntryPut && type != rocksdb::kEntryTimedPut) {
            return rocksdb::Status::OK();
        }
        const Clock::time_point t0 = Clock::now();
        try {
            // 在 SST 构建线程上只做“路由 + 必要数值解析 + 紧凑复制”。
            // 未采样行只做 O(1) 查表，不把 key/value 带入异步队列。
            capture_one_kv(*registry_, page_logging_,
                           key.data(), key.size(), value.data(), value.size(),
                           &page_, &value_scratch_, &last_slot_, &last_raw_pos_);
        } catch (...) {
            // RocksDB 明确要求异常不得越过 collector 边界。
            if (page_.first_error.empty()) {
                page_.first_error = "causal_meta: collector 捕获页面时发生未知异常";
            }
        }
        page_.capture_seconds += elapsed_seconds(t0, Clock::now());
        return rocksdb::Status::OK();
    }

    rocksdb::Status Finish(rocksdb::UserCollectedProperties*) override {
        const Clock::time_point t0 = Clock::now();
        try {
            G.stage_pending(token_, std::move(page_));
        } catch (...) {
        }
        // 这里必须无锁记账，不能等待正在运行 CDiT 的 state_mu_。
        G.add_finish_capture_seconds(elapsed_seconds(t0, Clock::now()));
        return rocksdb::Status::OK();
    }

    rocksdb::UserCollectedProperties GetReadableProperties() const override {
        return {};
    }

    const char* Name() const override { return "CausalMetaTableCollector"; }

private:
    std::uint64_t token_ = 0;
    std::shared_ptr<const CaptureRegistry> registry_;
    bool page_logging_ = false;
    PageCapture page_;
    std::string value_scratch_;
    int last_slot_ = -1;
    int last_raw_pos_ = -1;
};

class CausalMetaTableCollectorFactory final : public rocksdb::TablePropertiesCollectorFactory {
public:
    rocksdb::TablePropertiesCollector* CreateTablePropertiesCollector(Context) override {
        if (!G.enabled() || !tls_causal_flush || tls_causal_token == 0) return nullptr;
        try {
            auto registry = G.capture_registry_snapshot();
            return new CausalMetaTableCollector(tls_causal_token, std::move(registry), G.page_logging());
        } catch (...) {
            return nullptr;
        }
    }

    const char* Name() const override { return "CausalMetaTableCollectorFactory"; }
};

class CausalMetaEventListener final : public rocksdb::EventListener {
public:
    const char* Name() const override { return "CausalMetaEventListener"; }

    void OnTableFileCreationStarted(const rocksdb::TableFileCreationBriefInfo& info) override {
        try {
            if (G.enabled() && info.reason == rocksdb::TableFileCreationReason::kFlush) {
                tls_causal_flush = true;
                tls_causal_token = g_next_token.fetch_add(1, std::memory_order_relaxed);
            } else {
                tls_causal_flush = false;
                tls_causal_token = 0;
            }
        } catch (...) {
            tls_causal_flush = false;
            tls_causal_token = 0;
        }
    }

    void OnTableFileCreated(const rocksdb::TableFileCreationInfo& info) override {
        try {
            const std::uint64_t token = tls_causal_token;
            if (tls_causal_flush && token != 0) {
                G.commit_pending(token, info.status.ok());
            }
        } catch (...) {
        }
        tls_causal_flush = false;
        tls_causal_token = 0;
    }
};

std::shared_ptr<CausalMetaEventListener> g_listener = std::make_shared<CausalMetaEventListener>();
std::shared_ptr<CausalMetaTableCollectorFactory> g_factory = std::make_shared<CausalMetaTableCollectorFactory>();

#endif  // CAUSAL_META_NO_ROCKSDB

static std::vector<std::string> parse_tsv_line(const std::string& line) {
    return split_simple(line, '\t');
}

struct PersistedCData {
    std::string dataset_id;
    std::vector<std::string> variables;
    std::vector<MetaEntry> entries;
};

static bool load_c_file(const std::filesystem::path& path,
                        PersistedCData* out,
                        std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "无法打开 C 文件: " + path.string();
        return false;
    }
    std::string line;
    bool header_seen = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("#dataset\t", 0) == 0) {
            out->dataset_id = line.substr(std::string("#dataset\t").size());
        } else if (line.rfind("#variables\t", 0) == 0) {
            out->variables = split_simple(line.substr(std::string("#variables\t").size()), ',');
        } else if (!line.empty() && line[0] == '#') {
            continue;
        } else if (!header_seen) {
            header_seen = true;
            continue;
        } else if (!line.empty()) {
            const auto f = parse_tsv_line(line);
            if (f.size() < 8) continue;
            MetaEntry e;
            e.window_id = std::atoi(f[0].c_str());
            e.start_pos = std::atoi(f[1].c_str());
            e.end_pos = std::atoi(f[2].c_str());
            e.start_key = f[3];
            e.end_key = f[4];
            e.start_label = f[5];
            e.end_label = f[6];
            e.graph = graph_from_wire(f[7]);
            out->entries.push_back(std::move(e));
        }
    }
    if (out->dataset_id.empty()) {
        if (error) *error = "C 文件缺少 dataset 元信息: " + path.string();
        return false;
    }
    return true;
}

static bool load_t_index(const std::filesystem::path& path,
                         std::unordered_map<std::string, int>* positions,
                         int* max_pos,
                         std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "无法打开时间戳索引文件: " + path.string();
        return false;
    }
    std::string line;
    bool header_seen = false;
    *max_pos = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (!header_seen) {
            header_seen = true;
            continue;
        }
        auto f = parse_tsv_line(line);
        if (f.size() < 2) continue;
        const int p = std::atoi(f[0].c_str());
        (*positions)[f[1]] = p;
        *max_pos = std::max(*max_pos, p);
    }
    return true;
}

static CDiTResult project_graph(const CDiTResult& in,
                                const std::vector<std::string>& variables,
                                const std::vector<std::string>& query_columns) {
    if (query_columns.empty()) return in;
    std::unordered_set<int> keep;
    for (const std::string& q : query_columns) {
        for (std::size_t i = 0; i < variables.size(); ++i) {
            if (variables[i] == q) keep.insert(static_cast<int>(i) + 1);
        }
    }
    CDiTResult out;
    for (const auto& kv : in.lags_by_edge) {
        if (keep.count(kv.first.first) && keep.count(kv.first.second)) {
            out.lags_by_edge[kv.first] = kv.second;
        }
    }
    return out;
}

}  // 匿名命名空间结束

void causal_meta_reset_all() { G.reset(); }

void causal_meta_set_enabled_cpp(bool enabled) { G.set_enabled(enabled); }

bool causal_meta_is_enabled() { return G.enabled(); }

bool causal_meta_register_dataset(const CausalMetaDatasetConfig& cfg, std::string* error) {
    return G.register_dataset(cfg, error);
}

bool causal_meta_has_dataset(const std::string& dataset_id) { return G.has_dataset(dataset_id); }

std::vector<std::string> causal_meta_registered_dataset_ids() { return G.dataset_ids(); }

void causal_meta_set_metadata_dir(const std::string& dir) { G.set_metadata_dir(dir); }

std::string causal_meta_get_metadata_dir() { return G.metadata_dir(); }

void causal_meta_set_page_logging(bool enabled) { G.set_page_logging(enabled); }

void causal_meta_set_log_sink(void (*sink)(const char*, void*), void* userdata) {
    G.set_sink(sink, userdata);
}

void causal_meta_clear_log_sink() { G.clear_sink(); }

#ifndef CAUSAL_META_NO_ROCKSDB
bool causal_meta_attach_to_rocksdb_options(rocksdb::Options* options, std::string* error) {
    if (!options) {
        if (error) *error = "causal_meta_attach_to_rocksdb_options: options 为空";
        return false;
    }
    // 开关在 attach 前已关闭时，连 RocksDB listener/collector 都不注册：
    // 写入路径因此完全不进入本模块，供后续库外基线实验使用。
    if (!G.enabled()) return true;
    try {
        bool have_listener = false;
        for (const auto& p : options->listeners) {
            if (p.get() == g_listener.get()) {
                have_listener = true;
                break;
            }
        }
        if (!have_listener) options->listeners.push_back(g_listener);

        bool have_factory = false;
        for (const auto& p : options->table_properties_collector_factories) {
            if (p.get() == g_factory.get()) {
                have_factory = true;
                break;
            }
        }
        if (!have_factory) options->table_properties_collector_factories.push_back(g_factory);
        G.ensure_worker();
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    } catch (...) {
        if (error) *error = "附加 RocksDB 元数据扩展点时发生未知异常";
        return false;
    }
}
#endif

void causal_meta_wait_for_idle() { G.wait_idle(); }

bool causal_meta_persist_all(std::string* error) { return G.persist(error); }

bool causal_meta_before_db_close(std::string* error) {
    G.wait_idle();
    return G.persist(error);
}

CausalMetaCounters causal_meta_get_counters() { return G.counters(); }

double causal_meta_get_total_seconds() { return G.counters().metadata_seconds; }

std::string causal_meta_format_dataset_metadata(const std::string& dataset_id) {
    return G.format_metadata(dataset_id);
}

std::string causal_meta_format_dataset_buffers(const std::string& dataset_id) {
    return G.format_buffers(dataset_id);
}

std::vector<CausalMetaQuerySegment> causal_meta_query_dataset_from_file(
        const std::string& metadata_dir,
        const std::string& dataset_id,
        const std::string& query_start_key,
        const std::string& query_end_key,
        const std::vector<std::string>& query_columns,
        std::string* error) {
    std::vector<CausalMetaQuerySegment> out;
    const std::string base = safe_dataset_filename(dataset_id);
    PersistedCData cdata;
    if (!load_c_file(std::filesystem::path(metadata_dir) / (base + "_C.tsv"), &cdata, error)) {
        return out;
    }

    if (!query_columns.empty()) {
        std::unordered_set<std::string> known(cdata.variables.begin(), cdata.variables.end());
        for (const std::string& col : query_columns) {
            if (!known.count(col)) {
                if (error) *error = "范围查询列不存在: " + col;
                return {};
            }
        }
    }

    int qstart = 1;
    int qend = 0;
    std::unordered_map<std::string, int> positions;
    if (!query_start_key.empty() || !query_end_key.empty()) {
        if (!load_t_index(std::filesystem::path(metadata_dir) / (base + "_T.tsv"),
                          &positions, &qend, error)) {
            return {};
        }
        if (!query_start_key.empty()) {
            auto it = positions.find(query_start_key);
            if (it == positions.end()) {
                if (error) *error = "范围查询起点不是采样时间戳: " + query_start_key;
                return {};
            }
            qstart = it->second;
        }
        if (!query_end_key.empty()) {
            auto it = positions.find(query_end_key);
            if (it == positions.end()) {
                if (error) *error = "范围查询终点不是采样时间戳: " + query_end_key;
                return {};
            }
            qend = it->second;
        }
    } else {
        qstart = 1;
        qend = 0x7fffffff;
    }

    if (qstart > qend) {
        if (error) *error = "范围查询起点晚于终点";
        return {};
    }

    for (const MetaEntry& e : cdata.entries) {
        if (e.end_pos < qstart || e.start_pos > qend) continue;
        CausalMetaQuerySegment seg;
        seg.dataset_id = dataset_id;
        seg.start_key = e.start_key;
        seg.end_key = e.end_key;
        seg.start_label = e.start_label;
        seg.end_label = e.end_label;
        seg.variable_names = cdata.variables;
        seg.graph = project_graph(e.graph, cdata.variables, query_columns);
        out.push_back(std::move(seg));
    }
    return out;
}

std::string causal_meta_format_query_segment(
        const CausalMetaQuerySegment& seg,
        const std::vector<std::string>& preferred_order) {
    std::vector<std::string> order = preferred_order.empty() ? seg.variable_names : preferred_order;
    // 复用与页面/C 输出一致的变量排序逻辑。
    std::map<std::string, int> rank;
    for (std::size_t i = 0; i < order.size(); ++i) rank[order[i]] = static_cast<int>(i);
    auto name_of = [&](int idx) {
        if (idx >= 1 && idx <= static_cast<int>(seg.variable_names.size())) {
            return seg.variable_names[static_cast<std::size_t>(idx - 1)];
        }
        return std::string("X") + std::to_string(idx);
    };
    struct Item {
        int rp, rq, p, q;
        std::string ps, qs;
        std::vector<int> lags;
    };
    std::vector<Item> items;
    for (const auto& kv : seg.graph.lags_by_edge) {
        if (kv.second.empty()) continue;
        std::string ps = name_of(kv.first.first);
        std::string qs = name_of(kv.first.second);
        int rp = rank.count(ps) ? rank[ps] : 100000 + kv.first.first;
        int rq = rank.count(qs) ? rank[qs] : 100000 + kv.first.second;
        items.push_back({rp, rq, kv.first.first, kv.first.second, ps, qs, kv.second});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.rp != b.rp) return a.rp < b.rp;
        if (a.rq != b.rq) return a.rq < b.rq;
        if (a.p != b.p) return a.p < b.p;
        return a.q < b.q;
    });
    std::ostringstream oss;
    oss << seg.start_label << "至" << seg.end_label << " | 推断：";
    for (const Item& item : items) {
        std::vector<int> lags = item.lags;
        std::sort(lags.begin(), lags.end());
        lags.erase(std::unique(lags.begin(), lags.end()), lags.end());
        oss << item.ps << "→" << item.qs << "滞后=";
        for (int lag : lags) oss << lag << ',';
        oss << "; ";
    }
    return oss.str();
}

void causal_meta_submit_page_for_test(
        std::uint64_t page_id,
        const std::vector<std::pair<std::string, std::string>>& key_values) {
    G.submit_page_for_test(page_id, key_values);
}

extern "C" int causal_meta_enabled(void) { return G.enabled() ? 1 : 0; }

extern "C" void causal_meta_set_enabled(int enabled) { G.set_enabled(enabled != 0); }
