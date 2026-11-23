/*
 * 本文件维护 RemixDB 侧的数据集状态、窗口缓冲区和有序元数据；负责处理页面边界关系、更新窗口状态以及元数据持久化与查询。
 */

#include "causal_meta.h"
#include "merge_cg.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <time.h>
#include <unordered_map>
#include <utility>

namespace {

    struct MetaRow {
        int sample_pos = 0;
        std::string timestamp_key;
        std::string label;
        std::vector<double> values;
        bool fault = false;
    };

    struct BufferWindow {
        int window_id = 0;
        std::map<int, MetaRow> rows;
    };

    struct MetaEntry {
        int start_pos = 0;
        int end_pos = 0;
        std::string start_key;
        std::string end_key;
        std::string start_label;
        std::string end_label;
        CDiTResult graph;
    };

    struct DatasetState {
        CausalMetaDatasetConfig cfg;
        std::unordered_map<std::string, int> ts_to_sample_pos;
        std::unordered_map<std::string, std::string> ts_to_label;
        std::vector<std::string> sample_pos_to_key;
        std::vector<std::string> sample_pos_to_label;
        std::map<int, BufferWindow> buffers;
        std::vector<MetaEntry> entries;
        long long inferred_windows = 0;
        long long original_complete_windows = 0;
    };

    struct PageDatasetInfo {
        std::string dataset_id;
        std::string first_label;
        std::string last_label;
        bool has_any = false;
        bool complementary = false;
        bool overlapped = false;
        bool adjacent = false;
        std::set<int> touched_windows;
        std::set<int> preexisting_partial_windows;
    };

    struct PageContext {
        bool active = false;
        bool any_registered_kv = false;
        double cpu0 = 0.0;
        std::map<std::string, PageDatasetInfo> info_by_dataset;
    };

    struct Runtime {
        bool enabled = true;
        bool page_logging = false;
        std::string metadata_dir = "file/causal_experiment";
        std::map<std::string, DatasetState> datasets;
        PageContext page;
        CausalMetaCounters counters;
        void (*sink)(const char*, void*) = nullptr;
        void* sink_userdata = nullptr;
        std::mutex mu;
    } G;

    static double thread_cpu_seconds() {
#ifdef CLOCK_THREAD_CPUTIME_ID
        struct timespec ts;
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
            return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
        }
#endif
        return (double)std::clock() / (double)CLOCKS_PER_SEC;
    }

    static std::string trim(const std::string& s) {
        size_t l = 0, r = s.size();
        while (l < r && std::isspace((unsigned char)s[l])) ++l;
        while (r > l && std::isspace((unsigned char)s[r - 1])) --r;
        return s.substr(l, r - l);
    }

    static std::vector<std::string> split_simple(const std::string& s, char delim) {
        std::vector<std::string> out;
        std::string cur;
        std::istringstream iss(s);
        while (std::getline(iss, cur, delim)) out.push_back(cur);
        if (!s.empty() && s.back() == delim) out.push_back(std::string());
        return out;
    }

    static std::string join_strings(const std::vector<std::string>& xs, char delim) {
        std::ostringstream oss;
        for (size_t i = 0; i < xs.size(); ++i) {
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
            for (size_t i = 0; i < lags.size(); ++i) {
                if (i) oss << ',';
                oss << lags[i];
            }
        }
        return oss.str();
    }

    static CDiTResult graph_from_wire(const std::string& s) {
        CDiTResult r;
        for (const std::string& edge_s0 : split_simple(s, ';')) {
            std::string edge_s = trim(edge_s0);
            if (edge_s.empty()) continue;
            size_t gt = edge_s.find('>');
            size_t colon = edge_s.find(':');
            if (gt == std::string::npos || colon == std::string::npos || gt > colon) continue;
            int p = std::atoi(edge_s.substr(0, gt).c_str());
            int q = std::atoi(edge_s.substr(gt + 1, colon - gt - 1).c_str());
            std::vector<int> lags;
            for (const std::string& lag_s0 : split_simple(edge_s.substr(colon + 1), ',')) {
                std::string lag_s = trim(lag_s0);
                if (lag_s.empty()) continue;
                int v = std::atoi(lag_s.c_str());
                if (v > 0) lags.push_back(v);
            }
            std::sort(lags.begin(), lags.end());
            lags.erase(std::unique(lags.begin(), lags.end()), lags.end());
            if (!lags.empty()) r.lags_by_edge[std::make_pair(p, q)] = lags;
        }
        return r;
    }

    static std::vector<double> parse_values_fast(const char* data, size_t n) {
        std::vector<double> vals;
        vals.reserve(8);
        const char* p = data;
        const char* end = data + n;
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            char* q = nullptr;
            double v = std::strtod(p, &q);
            if (q == p) {
                v = 0.0;
                while (q < end && *q != ',') ++q;
            }
            vals.push_back(v);
            p = q;
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            if (p < end && *p == ',') ++p;
        }
        return vals;
    }

    static bool is_fault_values(const std::vector<double>& vals) {
        if (vals.empty()) return false;
        for (double v : vals) {
            if (std::fabs(v) > 1e-12) return false;
        }
        return true;
    }

    static bool buffer_has_fault(const BufferWindow& w) {
        for (const auto& kv : w.rows) if (kv.second.fault) return true;
        return false;
    }

    static std::string label_for_sample(const DatasetState& ds, int sample_pos) {
        if (sample_pos >= 1 && sample_pos < (int)ds.sample_pos_to_label.size()) return ds.sample_pos_to_label[(size_t)sample_pos];
        return std::string();
    }

    static std::string key_for_sample(const DatasetState& ds, int sample_pos) {
        if (sample_pos >= 1 && sample_pos < (int)ds.sample_pos_to_key.size()) return ds.sample_pos_to_key[(size_t)sample_pos];
        return std::string();
    }

    static void emit_log_unlocked(const std::string& msg) {
        if (G.sink) G.sink(msg.c_str(), G.sink_userdata);
    }

    static std::string format_graph_named_ordered(const CDiTResult& graph,
                                                  const std::vector<std::string>& names,
                                                  const std::vector<std::string>& preferred_order) {
        std::map<std::string, int> rank;
        for (size_t i = 0; i < preferred_order.size(); ++i) rank[preferred_order[i]] = (int)i;
        auto name_of = [&](int idx) -> std::string {
            if (idx >= 1 && idx <= (int)names.size() && !names[(size_t)idx - 1].empty()) return names[(size_t)idx - 1];
            std::ostringstream oss;
            oss << 'X' << idx;
            return oss.str();
        };
        struct Item { int rp, rq, p, q; std::string ps, qs; std::vector<int> lags; };
        std::vector<Item> items;
        for (const auto& kv : graph.lags_by_edge) {
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
        for (const auto& item : items) {
            std::vector<int> lags = item.lags;
            std::sort(lags.begin(), lags.end());
            oss << item.ps << "→" << item.qs << "滞后=";
            for (int v : lags) oss << v << ',';
            oss << "; ";
        }
        return oss.str();
    }

    static bool try_merge_entries(const DatasetState& ds, const MetaEntry& a, const MetaEntry& b, MetaEntry& out) {
        if (a.end_pos + 1 != b.start_pos) return false;

        const double merge_t0 = thread_cpu_seconds();
        MergeCGResult mr = merge_adjacent_causal_graphs(
                a.graph,
                b.graph,
                ds.cfg.tau,
                ds.cfg.delta,
                ds.cfg.theta,
                ds.cfg.ignore_self_in_merge);
        const double merge_t1 = thread_cpu_seconds();
        G.counters.merge_seconds += merge_t1 - merge_t0;

        if (!mr.merge) return false;

        out.start_pos = a.start_pos;
        out.end_pos = b.end_pos;
        out.start_key = a.start_key;
        out.end_key = b.end_key;
        out.start_label = a.start_label;
        out.end_label = b.end_label;
        out.graph = std::move(mr.merged);
        return true;
    }

    static bool has_adjacent_neighbor(const DatasetState& ds, size_t idx) {
        if (idx >= ds.entries.size()) return false;
        if (idx > 0 && ds.entries[idx - 1].end_pos + 1 == ds.entries[idx].start_pos) return true;
        if (idx + 1 < ds.entries.size() && ds.entries[idx].end_pos + 1 == ds.entries[idx + 1].start_pos) return true;
        return false;
    }

    static void insert_entry_with_local_merge(DatasetState& ds, MetaEntry entry, bool* page_adjacent) {
        auto it = std::lower_bound(ds.entries.begin(), ds.entries.end(), entry.start_pos,
                                   [](const MetaEntry& e, int s) { return e.start_pos < s; });
        size_t idx = (size_t)(it - ds.entries.begin());
        ds.entries.insert(it, std::move(entry));
        if (page_adjacent && has_adjacent_neighbor(ds, idx)) *page_adjacent = true;

        bool changed = true;
        while (changed && idx < ds.entries.size()) {
            changed = false;
            if (idx > 0) {
                MetaEntry merged;
                if (try_merge_entries(ds, ds.entries[idx - 1], ds.entries[idx], merged)) {
                    ds.entries.erase(ds.entries.begin() + (long)idx - 1, ds.entries.begin() + (long)idx + 1);
                    ds.entries.insert(ds.entries.begin() + (long)idx - 1, std::move(merged));
                    --idx;
                    changed = true;
                    continue;
                }
            }
            if (idx + 1 < ds.entries.size()) {
                MetaEntry merged;
                if (try_merge_entries(ds, ds.entries[idx], ds.entries[idx + 1], merged)) {
                    ds.entries.erase(ds.entries.begin() + (long)idx, ds.entries.begin() + (long)idx + 2);
                    ds.entries.insert(ds.entries.begin() + (long)idx, std::move(merged));
                    changed = true;
                    continue;
                }
            }
        }
    }

    static bool infer_buffer_window(DatasetState& ds, int window_id, bool* page_adjacent) {
        auto it = ds.buffers.find(window_id);
        if (it == ds.buffers.end()) return false;
        BufferWindow& bw = it->second;
        const int T = ds.cfg.window_length;
        if ((int)bw.rows.size() != T) return false;
        if (buffer_has_fault(bw)) return false;

        std::vector<std::vector<double>> data;
        data.reserve((size_t)T);
        const int start_pos = window_id * T + 1;
        const int end_pos = (window_id + 1) * T;
        for (int pos = start_pos; pos <= end_pos; ++pos) {
            auto rit = bw.rows.find(pos);
            if (rit == bw.rows.end()) return false;
            if ((int)rit->second.values.size() != (int)ds.cfg.variable_names.size()) return false;
            data.push_back(rit->second.values);
        }

        const std::vector<int>* src = ds.cfg.source_columns.empty() ? nullptr : &ds.cfg.source_columns;
        const std::vector<int>* tgt = ds.cfg.target_columns.empty() ? nullptr : &ds.cfg.target_columns;

        CDiTResult graph;
        if (ds.cfg.use_covariance_cache_cdit) {
            graph = run_cdit_covmeta(data, ds.cfg.tau, ds.cfg.alpha, ds.cfg.infer_self, src, tgt);
        } else {
            graph = run_cdit(data, ds.cfg.tau, ds.cfg.alpha, ds.cfg.infer_self, src, tgt);
        }

        MetaEntry e;
        e.start_pos = start_pos;
        e.end_pos = end_pos;
        e.start_key = key_for_sample(ds, start_pos);
        e.end_key = key_for_sample(ds, end_pos);
        e.start_label = label_for_sample(ds, start_pos);
        e.end_label = label_for_sample(ds, end_pos);
        e.graph = std::move(graph);
        ds.buffers.erase(it);
        ++ds.inferred_windows;
        insert_entry_with_local_merge(ds, std::move(e), page_adjacent);
        return true;
    }

    static std::string format_dataset_metadata_unlocked(const DatasetState& ds) {
        if (ds.entries.empty()) return "(无)";
        std::ostringstream oss;
        bool first = true;
        for (const MetaEntry& e : ds.entries) {
            if (!first) oss << ' ';
            first = false;
            oss << '[' << e.start_label << ", " << e.end_label << "],"
                << format_graph_named_ordered(e.graph, ds.cfg.variable_names, ds.cfg.variable_names);
        }
        return oss.str();
    }

    static std::string format_dataset_buffers_unlocked(const DatasetState& ds) {
        if (ds.buffers.empty()) return "(无)";
        std::ostringstream oss;
        bool first = true;
        for (const auto& kv : ds.buffers) {
            const BufferWindow& bw = kv.second;
            if (!first) oss << "; ";
            first = false;
            const int start_pos = bw.window_id * ds.cfg.window_length + 1;
            const int end_pos = (bw.window_id + 1) * ds.cfg.window_length;
            std::string status;
            const bool has_fault = buffer_has_fault(bw);
            if ((int)bw.rows.size() < ds.cfg.window_length) status = has_fault ? "不完整失败" : "不完整不失败";
            else status = has_fault ? "完整失败" : "完整不失败";
            std::string start_label = label_for_sample(ds, start_pos);
            std::string end_label;
            if (!bw.rows.empty()) end_label = bw.rows.rbegin()->second.label;
            if (end_label.empty()) end_label = label_for_sample(ds, end_pos);
            oss << start_label << "到" << end_label << '(' << status << ')';
        }
        return oss.str();
    }

    static void ensure_dir(const std::string& dir) {
        if (!dir.empty()) std::filesystem::create_directories(dir);
    }

    static std::string safe_filename(const std::string& x) {
        std::string out = x;
        for (char& c : out) {
            if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) c = '_';
        }
        return out;
    }

    static void persist_dataset_unlocked(const DatasetState& ds, const std::string& root) {
        ensure_dir(root);
        const std::string base = root + "/" + safe_filename(ds.cfg.dataset_id);
        {
            std::ofstream out(base + "_C.tsv", std::ios::binary | std::ios::trunc);
            out << "#dataset\t" << ds.cfg.dataset_id << "\n";
            out << "#display\t" << ds.cfg.display_name << "\n";
            out << "#vars\t" << join_strings(ds.cfg.variable_names, ',') << "\n";
            out << "#T\t" << ds.cfg.window_length << "\n";
            out << "#tau\t" << ds.cfg.tau << "\n";
            out << "#delta\t" << ds.cfg.delta << "\n";
            out << "#theta\t" << std::setprecision(17) << ds.cfg.theta << "\n";
            out << "#use_covariance_cache_cdit\t" << (ds.cfg.use_covariance_cache_cdit ? 1 : 0) << "\n";
            out << "start_pos\tend_pos\tstart_key\tend_key\tstart_label\tend_label\tgraph\n";
            for (const MetaEntry& e : ds.entries) {
                out << e.start_pos << '\t' << e.end_pos << '\t'
                    << e.start_key << '\t' << e.end_key << '\t'
                    << e.start_label << '\t' << e.end_label << '\t' << graph_to_wire(e.graph) << "\n";
            }
        }
        {
            std::ofstream out(base + "_B.tsv", std::ios::binary | std::ios::trunc);
            out << "#dataset\t" << ds.cfg.dataset_id << "\n";
            out << "window_id\trow_count\thas_fault\trows\n";
            for (const auto& kv : ds.buffers) {
                const BufferWindow& bw = kv.second;
                out << bw.window_id << '\t' << bw.rows.size() << '\t' << (buffer_has_fault(bw) ? 1 : 0) << '\t';
                bool first = true;
                for (const auto& rk : bw.rows) {
                    if (!first) out << ';';
                    first = false;
                    out << rk.second.sample_pos << ',' << rk.second.timestamp_key << ',' << (rk.second.fault ? 1 : 0);
                }
                out << "\n";
            }
        }
    }

    static std::vector<std::string> parse_vars_line(const std::string& line) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) return {};
        std::vector<std::string> vars;
        for (auto& s : split_simple(line.substr(tab + 1), ',')) vars.push_back(trim(s));
        return vars;
    }

    static int column_name_to_index(const std::vector<std::string>& vars, const std::string& name) {
        for (size_t i = 0; i < vars.size(); ++i) if (vars[i] == name) return (int)i + 1;
        if (name.size() >= 2 && (name[0] == 'X' || name[0] == 'x')) {
            int v = std::atoi(name.c_str() + 1);
            if (v >= 1 && v <= (int)vars.size()) return v;
        }
        return -1;
    }

    static CDiTResult project_graph(const CDiTResult& g, const std::set<int>& cols) {
        if (cols.empty()) return g;
        CDiTResult out;
        for (const auto& kv : g.lags_by_edge) {
            if (cols.count(kv.first.first) && cols.count(kv.first.second)) out.lags_by_edge[kv.first] = kv.second;
        }
        return out;
    }

    static void capture_preexisting_partial_windows_unlocked() {
        G.page.info_by_dataset.clear();
        for (const auto& dkv : G.datasets) {
            const DatasetState& ds = dkv.second;
            PageDatasetInfo pi;
            pi.dataset_id = dkv.first;
            for (const auto& bkv : ds.buffers) {
                const BufferWindow& bw = bkv.second;
                if (!bw.rows.empty() && (int)bw.rows.size() < ds.cfg.window_length) {
                    pi.preexisting_partial_windows.insert(bkv.first);
                }
            }
            if (!pi.preexisting_partial_windows.empty()) {
                G.page.info_by_dataset[dkv.first] = std::move(pi);
            }
        }
    }

    static PageDatasetInfo& page_info_for_dataset_unlocked(const std::string& dsid) {
        PageDatasetInfo& pi = G.page.info_by_dataset[dsid];
        if (pi.dataset_id.empty()) pi.dataset_id = dsid;
        return pi;
    }

    static void process_one_kv_unlocked(const char* kbuf, unsigned klen, const char* vbuf, unsigned vlen) {
        if (!G.page.active || !kbuf || klen == 0) return;

        const char* comma = (const char*)memchr(kbuf, ',', klen);
        if (!comma) return;
        std::string dsid(kbuf, (size_t)(comma - kbuf));
        auto dit = G.datasets.find(dsid);
        if (dit == G.datasets.end()) return;
        DatasetState& ds = dit->second;
        G.page.any_registered_kv = true;

        std::string ts_key(comma + 1, (size_t)(kbuf + klen - comma - 1));
        PageDatasetInfo& pi = page_info_for_dataset_unlocked(dsid);
        pi.has_any = true;

        if (G.page_logging) {
            auto lit = ds.ts_to_label.find(ts_key);
            std::string label = (lit == ds.ts_to_label.end()) ? ts_key : lit->second;
            if (pi.first_label.empty()) pi.first_label = label;
            pi.last_label = label;
        }

        auto sit = ds.ts_to_sample_pos.find(ts_key);
        if (sit == ds.ts_to_sample_pos.end()) return;
        const int sample_pos = sit->second;
        const int window_id = (sample_pos - 1) / ds.cfg.window_length;

        std::vector<double> vals = parse_values_fast(vbuf ? vbuf : "", (size_t)vlen);
        bool fault = is_fault_values(vals);

        BufferWindow& bw = ds.buffers[window_id];
        if (bw.rows.empty()) bw.window_id = window_id;

        auto old = bw.rows.find(sample_pos);
        if (old != bw.rows.end()) {
            if (old->second.fault && !fault) pi.overlapped = true;
            if (old->second.fault || fault) pi.overlapped = true;
        }

        MetaRow mr;
        mr.sample_pos = sample_pos;
        mr.timestamp_key = ts_key;
        auto lit = ds.ts_to_label.find(ts_key);
        mr.label = (lit == ds.ts_to_label.end()) ? ts_key : lit->second;
        mr.values = std::move(vals);
        mr.fault = fault;
        bw.rows[sample_pos] = std::move(mr);
        pi.touched_windows.insert(window_id);
    }

    static void finalize_page_unlocked() {
        if (!G.page.active || !G.page.any_registered_kv) return;

        for (auto& pikv : G.page.info_by_dataset) {
            const std::string& dsid = pikv.first;
            PageDatasetInfo& pi = pikv.second;
            auto dit = G.datasets.find(dsid);
            if (dit == G.datasets.end()) continue;
            DatasetState& ds = dit->second;

            for (int wid : pi.touched_windows) {
                if (pi.preexisting_partial_windows.count(wid)) {
                    auto bit = ds.buffers.find(wid);
                    if (bit != ds.buffers.end() && (int)bit->second.rows.size() == ds.cfg.window_length) {
                        pi.complementary = true;
                    }
                }
            }
            for (int wid : pi.touched_windows) {
                infer_buffer_window(ds, wid, &pi.adjacent);
            }
        }

        ++G.counters.total_pages;
        bool any_comp = false, any_ov = false;
        for (const auto& kv : G.page.info_by_dataset) {
            any_comp = any_comp || kv.second.complementary;
            any_ov = any_ov || kv.second.overlapped;
        }
        if (any_comp) ++G.counters.complementary_pages;
        if (any_ov) ++G.counters.overlapped_pages;

        if (G.page_logging) {
            std::ostringstream first;
            first << "[ ";
            bool first_item = true;
            for (const auto& kv : G.page.info_by_dataset) {
                const PageDatasetInfo& pi = kv.second;
                if (!pi.has_any) continue;
                if (!first_item) first << " | ";
                first_item = false;
                first << pi.dataset_id << "的" << pi.first_label << "到" << pi.last_label;
            }
            first << " ]";
            if (any_comp) first << " 互补页面";
            if (any_ov) first << " 重叠页面";
            bool any_adj = false;
            for (const auto& kv : G.page.info_by_dataset) any_adj = any_adj || kv.second.adjacent;
            if (any_adj) first << " 相邻页面";
            first << "\n";
            emit_log_unlocked(first.str());

            for (const auto& kv : G.page.info_by_dataset) {
                const std::string& dsid = kv.first;
                auto dit2 = G.datasets.find(dsid);
                if (dit2 != G.datasets.end() && kv.second.has_any) {
                    emit_log_unlocked(dsid + "的元数据：" + format_dataset_metadata_unlocked(dit2->second) + "\n");
                    emit_log_unlocked(dsid + "的暂存区：" + format_dataset_buffers_unlocked(dit2->second) + "\n");
                }
            }
        }
    }

} // 匿名命名空间结束

// 清空当前进程中的数据集、页面上下文、计数器和日志回调，并恢复默认开关状态。
void causal_meta_reset_all() {
    std::lock_guard<std::mutex> lk(G.mu);
    G.datasets.clear();
    G.page = PageContext();
    G.counters = CausalMetaCounters();
    G.sink = nullptr;
    G.sink_userdata = nullptr;
    G.page_logging = false;
    G.enabled = true;
}

void causal_meta_set_enabled_cpp(bool enabled) {
    causal_meta_set_enabled(enabled ? 1 : 0);
}

// 校验并注册数据集配置，同时建立原始时间戳到采样位置的映射。
bool causal_meta_register_dataset(const CausalMetaDatasetConfig& cfg_in) {
    std::lock_guard<std::mutex> lk(G.mu);
    if (cfg_in.dataset_id.empty() || cfg_in.variable_names.empty() || cfg_in.window_length <= 0 || cfg_in.sampling_step <= 0) return false;
    DatasetState ds;
    ds.cfg = cfg_in;
    if (ds.cfg.display_name.empty()) ds.cfg.display_name = ds.cfg.dataset_id;
    if (!ds.cfg.metadata_dir.empty()) G.metadata_dir = ds.cfg.metadata_dir;
    ds.sample_pos_to_key.push_back(std::string());
    ds.sample_pos_to_label.push_back(std::string());
    int raw_index = 0;
    int sample_pos = 0;
    std::set<std::string> seen;
    for (const auto& t : cfg_in.ordered_timestamps) {
        if (t.key.empty()) continue;
        if (!seen.insert(t.key).second) continue;
        ++raw_index;
        ds.ts_to_label[t.key] = t.label.empty() ? t.key : t.label;
        if (((raw_index - 1) % cfg_in.sampling_step) == 0) {
            ++sample_pos;
            ds.ts_to_sample_pos[t.key] = sample_pos;
            ds.sample_pos_to_key.push_back(t.key);
            ds.sample_pos_to_label.push_back(t.label.empty() ? t.key : t.label);
        }
    }
    ds.original_complete_windows = sample_pos / cfg_in.window_length;
    G.datasets[ds.cfg.dataset_id] = std::move(ds);
    return true;
}

bool causal_meta_has_dataset(const std::string& dataset_id) {
    std::lock_guard<std::mutex> lk(G.mu);
    return G.datasets.find(dataset_id) != G.datasets.end();
}

void causal_meta_set_page_logging(bool enabled) {
    std::lock_guard<std::mutex> lk(G.mu);
    G.page_logging = enabled;
}

void causal_meta_set_log_sink(void (*sink)(const char*, void*), void* userdata) {
    std::lock_guard<std::mutex> lk(G.mu);
    G.sink = sink;
    G.sink_userdata = userdata;
}

void causal_meta_clear_log_sink() {
    std::lock_guard<std::mutex> lk(G.mu);
    G.sink = nullptr;
    G.sink_userdata = nullptr;
}

void causal_meta_set_metadata_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(G.mu);
    G.metadata_dir = dir;
}

// 将每个已注册数据集的有序元数据和暂存窗口写入持久化目录。
void causal_meta_persist_all() {
    const double t0 = thread_cpu_seconds();
    std::lock_guard<std::mutex> lk(G.mu);
    const std::string root = G.metadata_dir;
    ensure_dir(root);
    for (const auto& kv : G.datasets) persist_dataset_unlocked(kv.second, root);
    G.counters.metadata_seconds += thread_cpu_seconds() - t0;
}

CausalMetaCounters causal_meta_get_counters() {
    std::lock_guard<std::mutex> lk(G.mu);
    CausalMetaCounters c = G.counters;
    c.inferred_windows = 0;
    c.original_complete_windows = 0;
    c.final_metadata_entries = 0;
    for (const auto& kv : G.datasets) {
        c.inferred_windows += kv.second.inferred_windows;
        c.original_complete_windows += kv.second.original_complete_windows;
        c.final_metadata_entries += (long long)kv.second.entries.size();
    }
    return c;
}

double causal_meta_get_total_seconds() {
    return causal_meta_get_counters().metadata_seconds;
}

std::string causal_meta_format_dataset_metadata(const std::string& dataset_id) {
    std::lock_guard<std::mutex> lk(G.mu);
    auto it = G.datasets.find(dataset_id);
    if (it == G.datasets.end()) return "(无)";
    return format_dataset_metadata_unlocked(it->second);
}

std::string causal_meta_format_dataset_buffers(const std::string& dataset_id) {
    std::lock_guard<std::mutex> lk(G.mu);
    auto it = G.datasets.find(dataset_id);
    if (it == G.datasets.end()) return "(无)";
    return format_dataset_buffers_unlocked(it->second);
}

std::vector<std::string> causal_meta_registered_dataset_ids() {
    std::lock_guard<std::mutex> lk(G.mu);
    std::vector<std::string> ids;
    for (const auto& kv : G.datasets) ids.push_back(kv.first);
    return ids;
}

// 仅从持久化元数据文件读取查询结果，不重新扫描数据库页面或原始时间序列。
std::vector<CausalMetaQuerySegment> causal_meta_query_dataset_from_file(
        const std::string& metadata_dir,
        const std::string& dataset_id,
        const std::string& query_start_key,
        const std::string& query_end_key,
        const std::vector<std::string>& query_columns) {
    std::vector<CausalMetaQuerySegment> out;
    std::ifstream in(metadata_dir + "/" + safe_filename(dataset_id) + "_C.tsv", std::ios::binary);
    if (!in.good()) return out;

    std::vector<std::string> vars;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("#vars\t", 0) == 0) {
            vars = parse_vars_line(line);
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("start_pos\t", 0) == 0) continue;
        std::vector<std::string> parts = split_simple(line, '\t');
        if (parts.size() < 6) continue;

        std::string start_key = parts.size() > 2 ? parts[2] : std::string();
        std::string end_key = parts.size() > 3 ? parts[3] : std::string();
        std::string start_label = parts.size() > 4 ? parts[4] : start_key;
        std::string end_label = parts.size() > 5 ? parts[5] : end_key;
        std::string graph_wire = parts.size() > 6 ? parts[6] : std::string();

        bool overlap = true;
        if (!query_start_key.empty() && query_start_key != "-∞" && end_key < query_start_key) overlap = false;
        if (!query_end_key.empty() && query_end_key != "+∞" && start_key > query_end_key) overlap = false;
        if (!overlap) continue;

        std::set<int> cols;
        if (!query_columns.empty()) {
            for (const std::string& c : query_columns) {
                int idx = column_name_to_index(vars, c);
                if (idx > 0) cols.insert(idx);
            }
        }
        CausalMetaQuerySegment seg;
        seg.dataset_id = dataset_id;
        seg.start_label = start_label;
        seg.end_label = end_label;
        seg.graph = project_graph(graph_from_wire(graph_wire), cols);
        out.push_back(std::move(seg));
    }
    return out;
}

std::string causal_meta_format_query_segment(const CausalMetaQuerySegment& seg,
                                             const std::vector<std::string>& preferred_order) {
    std::vector<std::string> names = preferred_order;
    if (names.empty()) names = {"X1", "X2", "X3"};
    std::ostringstream oss;
    oss << seg.start_label << "至" << seg.end_label << " | 推断："
        << format_graph_named_ordered(seg.graph, names, names);
    return oss.str();
}

extern "C" int causal_meta_xdb_enabled(void) {
    std::lock_guard<std::mutex> lk(G.mu);
    return (G.enabled && !G.datasets.empty()) ? 1 : 0;
}

extern "C" void causal_meta_set_enabled(int enabled) {
    std::lock_guard<std::mutex> lk(G.mu);
    G.enabled = enabled != 0;
}

// 页面扫描开始：持有全局互斥锁，确保同一时刻只有一个页面更新共享窗口状态。
extern "C" void causal_meta_xdb_begin_page(void) {
    // 整个 MemTable 页面扫描必须作为一个临界区执行。
    // RemixDB 的后台 compaction worker 与显式 flush/close 都可能触发页面扫描；
    // 若只在 begin 阶段短暂加锁，而 add_kv/end_page 不受保护，则多个扫描会共享并交错改写 G.page、G.datasets[*].buffers，
    // 进而破坏 std::map 内部结构，表现为 _Rb_tree_rebalance_for_erase 处段错误。
    G.mu.lock();
    if (!G.enabled || G.datasets.empty()) {
        G.mu.unlock();
        return;
    }
    G.page = PageContext();
    G.page.active = true;
    G.page.cpu0 = thread_cpu_seconds();
    capture_preexisting_partial_windows_unlocked();
}

// 页面扫描期间逐条提交键值记录；锁由 begin_page() 获取，由 end_page() 释放。
extern "C" void causal_meta_xdb_add_kv(const void* kbuf, unsigned klen, const void* vbuf, unsigned vlen) {
    // 调用约定：本函数只会在 causal_meta_xdb_begin_page() 和 causal_meta_xdb_end_page() 之间调用，
    // 此时 G.mu 已由 begin_page 持有，因此这里不要再次加锁，否则会自锁。
    if (!G.page.active || !kbuf) return;
    process_one_kv_unlocked((const char*)kbuf, klen, (const char*)vbuf, vlen);
}

// 页面扫描结束：完成窗口状态更新、统计耗时并释放页面扫描锁。
extern "C" void causal_meta_xdb_end_page(void) {
    // 若 begin_page 因元数据模块关闭或未注册数据集而提前返回，则这里没有持有锁，也无需处理。
    if (!G.page.active) return;
    finalize_page_unlocked();
    G.counters.metadata_seconds += thread_cpu_seconds() - G.page.cpu0;
    G.page = PageContext();
    G.mu.unlock();
}

// 数据库关闭前的兜底持久化入口；未启用或未注册数据集时直接返回。
extern "C" void causal_meta_on_xdb_close(void) {
    if (!causal_meta_xdb_enabled()) return;
    causal_meta_persist_all();
}
