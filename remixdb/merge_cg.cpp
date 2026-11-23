/*
 * 本文件实现相邻窗口因果图的结构相似度、滞后相似度计算，以及满足阈值时的滞后集合合并。
 */

#include "merge_cg.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

// 过滤无效或超过 tau 的滞后值，并返回排序、去重后的集合。
static std::vector<int> unique_sorted_clip(const std::vector<int>& v, int tau) {
    std::vector<int> x;
    x.reserve(v.size());
    for (int a : v) {
        if (a <= 0) continue;
        if (tau > 0 && a > tau) continue;
        x.push_back(a);
    }
    std::sort(x.begin(), x.end());
    x.erase(std::unique(x.begin(), x.end()), x.end());
    return x;
}

// 提取图中的边集合；按配置决定是否忽略自因果边。
static std::set<std::pair<int,int>> edge_set_ignore_lag(const CDiTResult& r, bool ignore_self) {
    std::set<std::pair<int,int>> E;
    for (const auto& kv : r.lags_by_edge) {
        int p = kv.first.first, q = kv.first.second;
        if (ignore_self && p == q) continue;
        if (!kv.second.empty()) E.insert(kv.first);
    }
    return E;
}

// 计算两个有序边集合的交集大小。
static int intersection_size_pairset(const std::set<std::pair<int,int>>& A,
                                     const std::set<std::pair<int,int>>& B) {
    if (A.empty() || B.empty()) return 0;
    int cnt = 0; auto itA = A.begin(), itB = B.begin();
    while (itA != A.end() && itB != B.end()) {
        if (*itA < *itB) ++itA;
        else if (*itB < *itA) ++itB;
        else { ++cnt; ++itA; ++itB; }
    }
    return cnt;
}

// 返回两个有序边集合的交集。
static std::set<std::pair<int,int>> intersection_pairset(const std::set<std::pair<int,int>>& A,
                                                         const std::set<std::pair<int,int>>& B) {
    std::set<std::pair<int,int>> I;
    std::set_intersection(A.begin(), A.end(), B.begin(), B.end(), std::inserter(I, I.begin()));
    return I;
}

// 返回两个有序边集合的并集。
static std::set<std::pair<int,int>> union_pairset(const std::set<std::pair<int,int>>& A,
                                                  const std::set<std::pair<int,int>>& B) {
    std::set<std::pair<int,int>> U;
    std::set_union(A.begin(), A.end(), B.begin(), B.end(), std::inserter(U, U.begin()));
    return U;
}

// 根据边集合的交并比计算结构相似度。
static double jaccard_pairset(const std::set<std::pair<int,int>>& A,
                              const std::set<std::pair<int,int>>& B) {
    auto U = union_pairset(A, B);
    if (U.empty()) return 1.0;
    int inter = intersection_size_pairset(A, B);
    return (double)inter / (double)U.size();
}

struct LagCluster { std::vector<int> elems; bool in1 = false; bool in2 = false; };

// 将相邻间隔不超过 Delta 的滞后值划分为簇，并记录各簇来自哪一个输入图。
static std::vector<LagCluster> build_clusters(const std::vector<int>& union_lags_sorted_unique,
                                              const std::set<int>& s1, const std::set<int>& s2, int Delta) {
    std::vector<LagCluster> clusters;
    if (union_lags_sorted_unique.empty()) return clusters;
    LagCluster cur;
    cur.elems.push_back(union_lags_sorted_unique[0]);
    cur.in1 = s1.count(union_lags_sorted_unique[0]) > 0;
    cur.in2 = s2.count(union_lags_sorted_unique[0]) > 0;
    for (size_t i = 1; i < union_lags_sorted_unique.size(); ++i) {
        int prev = union_lags_sorted_unique[i - 1], now = union_lags_sorted_unique[i];
        if (now - prev <= Delta) {
            cur.elems.push_back(now);
            if (s1.count(now)) cur.in1 = true;
            if (s2.count(now)) cur.in2 = true;
        } else {
            clusters.push_back(cur);
            cur = LagCluster();
            cur.elems.push_back(now);
            cur.in1 = s1.count(now) > 0;
            cur.in2 = s2.count(now) > 0;
        }
    }
    clusters.push_back(cur);
    return clusters;
}

// 计算当前簇与最近其他簇之间的滞后间隔。
static double distance_to_nearest_other_cluster(const std::vector<LagCluster>& clusters, int k) {
    int K = (int)clusters.size();
    if (K <= 1) return std::numeric_limits<double>::infinity();
    double dist = std::numeric_limits<double>::infinity();
    int x = clusters[k].elems.front();
    if (k - 1 >= 0) dist = std::min(dist, (double)(x - clusters[k - 1].elems.back()));
    if (k + 1 < K) dist = std::min(dist, (double)(clusters[k + 1].elems.front() - x));
    return dist;
}

// 对一条共有边比较两个滞后集合；先聚类，再计算簇级 Jaccard 相似度。
static double sim_lag_one_edge(const std::vector<int>& lags1, const std::vector<int>& lags2,
                               int Delta, int tau) {
    auto v1 = unique_sorted_clip(lags1, tau), v2 = unique_sorted_clip(lags2, tau);
    std::set<int> s1(v1.begin(), v1.end()), s2(v2.begin(), v2.end());
    std::vector<int> uni;
    uni.reserve(v1.size() + v2.size());
    std::set_union(v1.begin(), v1.end(), v2.begin(), v2.end(), std::back_inserter(uni));
    auto clusters = build_clusters(uni, s1, s2, Delta);
    int K = (int)clusters.size();
    std::set<int> isolated;
    if (K >= 2) {
        for (int k = 0; k < K; ++k) {
            const auto& c = clusters[k];
            if ((int)c.elems.size() != 1) continue;
            int a = c.in1 ? 1 : 0, b = c.in2 ? 1 : 0;
            if (a + b != 1) continue;
            if (distance_to_nearest_other_cluster(clusters, k) > (double)Delta) isolated.insert(k);
        }
    }
    std::set<int> K1, K2;
    for (int k = 0; k < K; ++k) {
        if (isolated.count(k)) continue;
        if (clusters[k].in1) K1.insert(k);
        if (clusters[k].in2) K2.insert(k);
    }
    if (K1.empty() && K2.empty()) return 1.0;
    int inter = 0; for (int x : K1) if (K2.count(x)) ++inter;
    int uni_sz = (int)K1.size() + (int)K2.size() - inter;
    if (uni_sz <= 0) return 1.0;
    return (double)inter / (double)uni_sz;
}

// 对滞后簇进行压缩，保留簇中心附近的代表性滞后值。
static std::vector<int> compress_cluster_keep_center(const std::vector<int>& c, int Delta) {
    std::vector<int> keep;
    if (c.empty()) return keep;
    int w = c.back() - c.front(), n = (int)c.size();
    std::vector<int> centers;
    if (n % 2 == 1) centers.push_back(c[n / 2]);
    else { centers.push_back(c[n / 2 - 1]); centers.push_back(c[n / 2]); }
    if (w <= Delta) {
        keep = centers;
    } else {
        int radius = (int)std::floor((double)Delta / 2.0);
        for (int x : c) {
            int best = std::numeric_limits<int>::max();
            for (int ctr : centers) best = std::min(best, std::abs(x - ctr));
            if (best <= radius) keep.push_back(x);
        }
        if (keep.empty()) keep = centers;
    }
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    return keep;
}

// 合并一条共有边的滞后集合，并去除无效或超出搜索上界的滞后。
static std::vector<int> merge_lags_one_edge(const std::vector<int>& lags1, const std::vector<int>& lags2,
                                            int Delta, int tau) {
    auto v1 = unique_sorted_clip(lags1, tau), v2 = unique_sorted_clip(lags2, tau);
    std::set<int> s1(v1.begin(), v1.end()), s2(v2.begin(), v2.end());
    std::vector<int> uni;
    uni.reserve(v1.size() + v2.size());
    std::set_union(v1.begin(), v1.end(), v2.begin(), v2.end(), std::back_inserter(uni));
    auto clusters = build_clusters(uni, s1, s2, Delta);
    int K = (int)clusters.size();
    std::set<int> isolated;
    if (K >= 2) {
        for (int k = 0; k < K; ++k) {
            const auto& c = clusters[k];
            if ((int)c.elems.size() != 1) continue;
            int a = c.in1 ? 1 : 0, b = c.in2 ? 1 : 0;
            if (a + b != 1) continue;
            if (distance_to_nearest_other_cluster(clusters, k) > (double)Delta) isolated.insert(k);
        }
    }
    std::vector<int> merged;
    for (int k = 0; k < K; ++k) {
        if (isolated.count(k)) continue;
        auto keep = compress_cluster_keep_center(clusters[k].elems, Delta);
        merged.insert(merged.end(), keep.begin(), keep.end());
    }
    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
    merged.erase(std::remove_if(merged.begin(), merged.end(), [&](int x){ return x <= 0 || (tau > 0 && x > tau); }), merged.end());
    return merged;
}

// 先计算结构相似度与共有边上的滞后相似度；达到阈值后，仅合并两图共有的边。
MergeCGResult merge_adjacent_causal_graphs(const CDiTResult& C1, const CDiTResult& C2,
                                           int tau, int Delta, double theta, bool ignore_self) {
    MergeCGResult out;
    auto P1 = edge_set_ignore_lag(C1, ignore_self);
    auto P2 = edge_set_ignore_lag(C2, ignore_self);
    auto Puni = union_pairset(P1, P2);
    auto Pint = intersection_pairset(P1, P2);
    double simStr = jaccard_pairset(P1, P2), simLag = 0.0;
    if (Puni.empty()) simLag = 1.0;
    else if (Pint.empty()) simLag = 0.0;
    else {
        simLag = 1.0;
        for (const auto& e : Pint) {
            auto it1 = C1.lags_by_edge.find(e), it2 = C2.lags_by_edge.find(e);
            std::vector<int> l1 = (it1 == C1.lags_by_edge.end()) ? std::vector<int>() : it1->second;
            std::vector<int> l2 = (it2 == C2.lags_by_edge.end()) ? std::vector<int>() : it2->second;
            simLag = std::min(simLag, sim_lag_one_edge(l1, l2, Delta, tau));
        }
    }
    double sim = simStr * simLag;
    out.sim_lag = simLag;
    out.similarity = sim;
    if (sim + 1e-12 < theta) { out.merge = 0; return out; }
    CDiTResult merged;
    for (const auto& e : Pint) {
        auto it1 = C1.lags_by_edge.find(e), it2 = C2.lags_by_edge.find(e);
        std::vector<int> l1 = (it1 == C1.lags_by_edge.end()) ? std::vector<int>() : it1->second;
        std::vector<int> l2 = (it2 == C2.lags_by_edge.end()) ? std::vector<int>() : it2->second;
        auto ml = merge_lags_one_edge(l1, l2, Delta, tau);
        if (!ml.empty()) merged.lags_by_edge[e] = std::move(ml);
    }
    out.merge = 1;
    out.merged = std::move(merged);
    return out;
}
