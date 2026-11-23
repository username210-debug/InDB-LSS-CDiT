/*
 * 本文件实现窗口级分析的两种计算路径；缓存路径复用预先构建的滞后统计量，减少重复的矩阵计算。
 */

#include "cdit_module.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <vector>

namespace {

    static const double kSingularTol = 1e-12;
    static const double kDecisionFallbackMargin = 1e-10;

    static std::vector<int> normalize_column_list(const std::vector<int>* cols, int d) {
        std::vector<int> out;
        if (!cols || cols->empty()) {
            out.reserve((size_t)d);
            for (int i = 1; i <= d; ++i) out.push_back(i);
            return out;
        }
        std::set<int> uniq;
        for (size_t i = 0; i < cols->size(); ++i) {
            int x = (*cols)[i];
            if (x >= 1 && x <= d) uniq.insert(x);
        }
        out.assign(uniq.begin(), uniq.end());
        return out;
    }

    static std::vector<std::vector<double>> transpose_to_dim_time(const std::vector<std::vector<double>>& data) {
        if (data.empty() || data[0].empty()) return std::vector<std::vector<double>>();
        const int T = (int)data.size();
        const int d = (int)data[0].size();
        std::vector<std::vector<double>> out(d, std::vector<double>(T, 0.0));
        for (int t = 0; t < T; ++t) {
            for (int j = 0; j < d; ++j) {
                out[j][t] = data[t][j];
            }
        }
        return out;
    }

    static double mean_of(const std::vector<double>& v) {
        if (v.empty()) return 0.0;
        double s = 0.0;
        for (size_t i = 0; i < v.size(); ++i) s += v[i];
        return s / (double)v.size();
    }

    static void center_in_place(std::vector<double>& v) {
        const double m = mean_of(v);
        for (size_t i = 0; i < v.size(); ++i) v[i] -= m;
    }

    static void center_columns_in_place(std::vector<std::vector<double>>& Z) {
        if (Z.empty() || Z[0].empty()) return;
        const int n = (int)Z.size();
        const int k = (int)Z[0].size();
        for (int j = 0; j < k; ++j) {
            double m = 0.0;
            for (int i = 0; i < n; ++i) m += Z[i][j];
            m /= (double)n;
            for (int i = 0; i < n; ++i) Z[i][j] -= m;
        }
    }

    static double dot(const std::vector<double>& a, const std::vector<double>& b) {
        double s = 0.0;
        for (size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
        return s;
    }

    static double norm2(const std::vector<double>& a) {
        return std::sqrt(std::max(0.0, dot(a, a)));
    }

    // 保留原始实现中的 PaCoT 残差计算路径，作为数值回退方案。
    // 当协方差子矩阵奇异时，使用该路径进行数值回退。
    // 当检验统计量非常接近判定边界时，也使用该路径避免数值误差改变判定结果。
    static std::vector<double> regress_residuals_with_intercept(
            const std::vector<double>& y,
            const std::vector<std::vector<double>>& Zcols_no_intercept) {
        const int n = (int)y.size();
        const int k0 = (int)Zcols_no_intercept.size();
        const int p = k0 + 1;

        std::vector<std::vector<double>> Acols(p, std::vector<double>(n, 1.0));
        for (int j = 0; j < k0; ++j) {
            Acols[j + 1] = Zcols_no_intercept[j];
        }

        std::vector<std::vector<double>> Q(p, std::vector<double>(n, 0.0));
        std::vector<std::vector<double>> R(p, std::vector<double>(p, 0.0));

        for (int j = 0; j < p; ++j) {
            std::vector<double> v = Acols[j];
            for (int i = 0; i < j; ++i) {
                R[i][j] = dot(Q[i], v);
                for (int r = 0; r < n; ++r) v[r] -= R[i][j] * Q[i][r];
            }
            R[j][j] = norm2(v);
            if (R[j][j] <= 1e-12) {
                std::fill(Q[j].begin(), Q[j].end(), 0.0);
                R[j][j] = 0.0;
            } else {
                for (int r = 0; r < n; ++r) Q[j][r] = v[r] / R[j][j];
            }
        }

        std::vector<double> qty(p, 0.0), beta(p, 0.0);
        for (int j = 0; j < p; ++j) qty[j] = dot(Q[j], y);
        for (int i = p - 1; i >= 0; --i) {
            if (std::fabs(R[i][i]) <= 1e-12) {
                beta[i] = 0.0;
                continue;
            }
            double s = qty[i];
            for (int j = i + 1; j < p; ++j) s -= R[i][j] * beta[j];
            beta[i] = s / R[i][i];
        }

        std::vector<double> res(n, 0.0);
        for (int i = 0; i < n; ++i) {
            double yhat = beta[0];
            for (int j = 0; j < k0; ++j) yhat += beta[j + 1] * Zcols_no_intercept[j][i];
            res[i] = y[i] - yhat;
        }
        return res;
    }

    static double pearson_corr_like_matlab(const std::vector<double>& a, const std::vector<double>& b) {
        const int n = (int)a.size();
        if (n <= 1 || (int)b.size() != n) return 0.0;

        double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
        for (int i = 0; i < n; ++i) {
            sa += a[i];
            sb += b[i];
            saa += a[i] * a[i];
            sbb += b[i] * b[i];
            sab += a[i] * b[i];
        }

        const double num = n * sab - sa * sb;
        const double den_a = n * saa - sa * sa;
        const double den_b = n * sbb - sb * sb;
        if (den_a <= 0.0 || den_b <= 0.0) return 0.0;

        return num / std::sqrt(den_a * den_b);
    }

    static double inverse_standard_normal_cdf(double p) {
        if (p <= 0.0) return -std::numeric_limits<double>::infinity();
        if (p >= 1.0) return std::numeric_limits<double>::infinity();

        static const double a1 = -3.969683028665376e+01;
        static const double a2 =  2.209460984245205e+02;
        static const double a3 = -2.759285104469687e+02;
        static const double a4 =  1.383577518672690e+02;
        static const double a5 = -3.066479806614716e+01;
        static const double a6 =  2.506628277459239e+00;

        static const double b1 = -5.447609879822406e+01;
        static const double b2 =  1.615858368580409e+02;
        static const double b3 = -1.556989798598866e+02;
        static const double b4 =  6.680131188771972e+01;
        static const double b5 = -1.328068155288572e+01;

        static const double c1 = -7.784894002430293e-03;
        static const double c2 = -3.223964580411365e-01;
        static const double c3 = -2.400758277161838e+00;
        static const double c4 = -2.549732539343734e+00;
        static const double c5 =  4.374664141464968e+00;
        static const double c6 =  2.938163982698783e+00;

        static const double d1 =  7.784695709041462e-03;
        static const double d2 =  3.224671290700398e-01;
        static const double d3 =  2.445134137142996e+00;
        static const double d4 =  3.754408661907416e+00;

        const double plow = 0.02425;
        const double phigh = 1.0 - plow;

        if (p < plow) {
            const double q = std::sqrt(-2.0 * std::log(p));
            return (((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
                   ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
        }
        if (p > phigh) {
            const double q = std::sqrt(-2.0 * std::log(1.0 - p));
            return -(((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
                   ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
        }

        const double q = p - 0.5;
        const double r = q * q;
        return (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6) * q /
               (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
    }

    static bool pacot_independent_reference(const std::vector<double>& x_in,
                                            const std::vector<double>& y_in,
                                            const std::vector<std::vector<double>>& Z_in,
                                            double alpha) {
        const int n = (int)x_in.size();
        if (n == 0 || (int)y_in.size() != n) return true;
        if (!Z_in.empty() && (int)Z_in.size() != n) return true;

        std::vector<double> x = x_in;
        std::vector<double> y = y_in;
        center_in_place(x);
        center_in_place(y);

        double pcc = 0.0;
        int ncit = 0;

        if (Z_in.empty() || Z_in[0].empty()) {
            ncit = 0;
            pcc = pearson_corr_like_matlab(x, y);
        } else {
            std::vector<std::vector<double>> Z = Z_in;
            center_columns_in_place(Z);
            ncit = (int)Z[0].size();

            std::vector<std::vector<double>> Zcols(ncit, std::vector<double>(n, 0.0));
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j < ncit; ++j) {
                    Zcols[j][i] = Z[i][j];
                }
            }

            std::vector<double> rx = regress_residuals_with_intercept(x, Zcols);
            std::vector<double> ry = regress_residuals_with_intercept(y, Zcols);
            pcc = pearson_corr_like_matlab(rx, ry);
        }

        if (pcc >= 1.0) pcc = 1.0 - 1e-12;
        if (pcc <= -1.0) pcc = -1.0 + 1e-12;

        const double dof = (double)n - (double)ncit - 3.0;
        if (dof <= 0.0 || !std::isfinite(dof)) return true;

        const double zpcc = 0.5 * std::log((1.0 + pcc) / (1.0 - pcc));
        const double A = std::sqrt(dof) * std::fabs(zpcc);
        const double B = inverse_standard_normal_cdf(1.0 - alpha / 2.0);

        if (!std::isfinite(A) || !std::isfinite(B)) return true;
        return !(A > B);
    }

    static bool pacot_dependent_reference(const std::vector<double>& x,
                                          const std::vector<double>& y,
                                          const std::vector<std::vector<double>>& Z,
                                          double alpha) {
        return !pacot_independent_reference(x, y, Z, alpha);
    }

    struct LaggedCovCache {
        int n;
        int d;
        int max_lag;
        std::vector<std::vector<double>> L;       // 行索引对应样本，列索引对应滞后变量。
        std::vector<double> mean;                 // 列索引对应滞后变量。
        std::vector<std::vector<double>> scatter; // 中心化交叉乘积，尚未除以样本数 n。

        int col_id(int var0, int lag) const {
            return var0 * (max_lag + 1) + lag;
        }

        std::vector<double> column(int cid) const {
            std::vector<double> out((size_t)n, 0.0);
            for (int i = 0; i < n; ++i) out[i] = L[i][cid];
            return out;
        }
    };

    static LaggedCovCache build_lagged_cov_cache(const std::vector<std::vector<double>>& dt,
                                                 int T,
                                                 int d,
                                                 int tau) {
        LaggedCovCache c;
        c.d = d;
        c.max_lag = 2 * tau - 1;
        const int s = 2 * tau;
        const int t = T;
        c.n = t - s + 1;
        const int P = d * (c.max_lag + 1);

        c.L.assign((size_t)c.n, std::vector<double>((size_t)P, 0.0));
        for (int row = 0, col = s; col <= t; ++col, ++row) {
            for (int var = 0; var < d; ++var) {
                for (int lag = 0; lag <= c.max_lag; ++lag) {
                    c.L[row][c.col_id(var, lag)] = dt[var][col - lag - 1];
                }
            }
        }

        c.mean.assign((size_t)P, 0.0);
        for (int j = 0; j < P; ++j) {
            double ssum = 0.0;
            for (int i = 0; i < c.n; ++i) ssum += c.L[i][j];
            c.mean[j] = ssum / (double)c.n;
        }

        c.scatter.assign((size_t)P, std::vector<double>((size_t)P, 0.0));
        for (int a = 0; a < P; ++a) {
            for (int b = a; b < P; ++b) {
                double ssum = 0.0;
                for (int i = 0; i < c.n; ++i) {
                    ssum += (c.L[i][a] - c.mean[a]) * (c.L[i][b] - c.mean[b]);
                }
                c.scatter[a][b] = ssum;
                c.scatter[b][a] = ssum;
            }
        }
        return c;
    }

    static bool invert_matrix(const std::vector<std::vector<double>>& A,
                              std::vector<std::vector<double>>& inv) {
        const int n = (int)A.size();
        if (n == 0) return false;
        for (int i = 0; i < n; ++i) {
            if ((int)A[i].size() != n) return false;
        }

        std::vector<std::vector<double>> aug(n, std::vector<double>((size_t)(2 * n), 0.0));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) aug[i][j] = A[i][j];
            aug[i][n + i] = 1.0;
        }

        for (int col = 0; col < n; ++col) {
            int pivot = col;
            double best = std::fabs(aug[col][col]);
            for (int r = col + 1; r < n; ++r) {
                const double v = std::fabs(aug[r][col]);
                if (v > best) {
                    best = v;
                    pivot = r;
                }
            }
            if (best <= kSingularTol || !std::isfinite(best)) return false;
            if (pivot != col) std::swap(aug[pivot], aug[col]);

            const double diag = aug[col][col];
            for (int j = 0; j < 2 * n; ++j) aug[col][j] /= diag;

            for (int r = 0; r < n; ++r) {
                if (r == col) continue;
                const double factor = aug[r][col];
                if (factor == 0.0) continue;
                for (int j = 0; j < 2 * n; ++j) {
                    aug[r][j] -= factor * aug[col][j];
                }
            }
        }

        inv.assign((size_t)n, std::vector<double>((size_t)n, 0.0));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) inv[i][j] = aug[i][n + j];
        }
        return true;
    }

    static bool pacot_independent_from_columns_fallback(const LaggedCovCache& cache,
                                                        int x_col,
                                                        int y_col,
                                                        const std::vector<int>& z_cols,
                                                        double alpha) {
        std::vector<double> x = cache.column(x_col);
        std::vector<double> y = cache.column(y_col);
        std::vector<std::vector<double>> Z;
        if (!z_cols.empty()) {
            Z.assign((size_t)cache.n, std::vector<double>(z_cols.size(), 0.0));
            for (int i = 0; i < cache.n; ++i) {
                for (size_t j = 0; j < z_cols.size(); ++j) {
                    Z[i][j] = cache.L[i][z_cols[j]];
                }
            }
        }
        return pacot_independent_reference(x, y, Z, alpha);
    }

    static bool pacot_independent_cached(const LaggedCovCache& cache,
                                         int x_col,
                                         int y_col,
                                         const std::vector<int>& z_cols,
                                         double alpha) {
        const int n = cache.n;
        const int ncit = (int)z_cols.size();
        const double dof = (double)n - (double)ncit - 3.0;
        if (dof <= 0.0 || !std::isfinite(dof)) return true;

        double pcc = 0.0;

        if (z_cols.empty()) {
            const double sxx = cache.scatter[x_col][x_col];
            const double syy = cache.scatter[y_col][y_col];
            const double sxy = cache.scatter[x_col][y_col];
            if (sxx <= 0.0 || syy <= 0.0) {
                pcc = 0.0;
            } else {
                pcc = sxy / std::sqrt(sxx * syy);
            }
        } else {
            std::vector<int> ids;
            ids.reserve((size_t)ncit + 2);
            ids.push_back(x_col);
            ids.push_back(y_col);
            for (size_t i = 0; i < z_cols.size(); ++i) ids.push_back(z_cols[i]);

            const int m = (int)ids.size();
            std::vector<std::vector<double>> sub((size_t)m, std::vector<double>((size_t)m, 0.0));
            for (int i = 0; i < m; ++i) {
                for (int j = 0; j < m; ++j) {
                    sub[i][j] = cache.scatter[ids[i]][ids[j]];
                }
            }

            std::vector<std::vector<double>> omega;
            if (!invert_matrix(sub, omega)) {
                return pacot_independent_from_columns_fallback(cache, x_col, y_col, z_cols, alpha);
            }

            const double oxo = omega[0][0];
            const double oyo = omega[1][1];
            if (oxo <= 0.0 || oyo <= 0.0 || !std::isfinite(oxo) || !std::isfinite(oyo)) {
                return pacot_independent_from_columns_fallback(cache, x_col, y_col, z_cols, alpha);
            }
            pcc = -omega[0][1] / std::sqrt(oxo * oyo);
        }

        if (!std::isfinite(pcc)) {
            return pacot_independent_from_columns_fallback(cache, x_col, y_col, z_cols, alpha);
        }
        if (pcc >= 1.0) pcc = 1.0 - 1e-12;
        if (pcc <= -1.0) pcc = -1.0 + 1e-12;

        const double zpcc = 0.5 * std::log((1.0 + pcc) / (1.0 - pcc));
        const double A = std::sqrt(dof) * std::fabs(zpcc);
        const double B = inverse_standard_normal_cdf(1.0 - alpha / 2.0);

        if (!std::isfinite(A) || !std::isfinite(B)) return true;

        // 在判定边界附近保留参考实现的决策结果；此处很小的数值差异也可能
        // 由 QR 残差化与精度矩阵求逆之间的计算差异引起，
        // 进而改变 A > B 的比较结果。
        if (std::fabs(A - B) <= kDecisionFallbackMargin) {
            return pacot_independent_from_columns_fallback(cache, x_col, y_col, z_cols, alpha);
        }

        return !(A > B);
    }

    static bool pacot_dependent_cached(const LaggedCovCache& cache,
                                       int x_col,
                                       int y_col,
                                       const std::vector<int>& z_cols,
                                       double alpha) {
        return !pacot_independent_cached(cache, x_col, y_col, z_cols, alpha);
    }

    static std::vector<int> get_allowed_self_nodes(bool infer_self,
                                                   const std::vector<int>& sources,
                                                   const std::vector<int>& targets) {
        std::vector<int> nodes;
        if (!infer_self) return nodes;

        std::set<int> target_set(targets.begin(), targets.end());
        for (size_t i = 0; i < sources.size(); ++i) {
            if (target_set.count(sources[i])) nodes.push_back(sources[i]);
        }
        return nodes;
    }

} // 匿名命名空间结束


CDiTResult run_cdit_original(const std::vector<std::vector<double>>& data,
                             int tau,
                             double alpha,
                             bool infer_self,
                             const std::vector<int>* source_columns,
                             const std::vector<int>* target_columns) {
    CDiTResult out;
    if (data.empty() || data[0].empty() || tau <= 0) return out;

    const int T = (int)data.size();
    const int d = (int)data[0].size();
    if (T < 2 * tau) return out;

    const std::vector<int> sources = normalize_column_list(source_columns, d);
    const std::vector<int> targets = normalize_column_list(target_columns, d);
    const std::vector<int> self_nodes = get_allowed_self_nodes(infer_self, sources, targets);

    const std::vector<std::vector<double>> dt = transpose_to_dim_time(data); // 矩阵按 [变量][时间] 组织。

    struct MinLagEdge {
        int src;
        int dst;
        int lag;
    };
    std::vector<MinLagEdge> min_lag_edges;

    // 与参考 MATLAB 实现保持一致：
    // s = 2*maxLag；t = size(d,2)。
    // MATLAB 使用从 1 开始的下标；本实现访问数组时统一转换为 C++ 的 0 基下标。
    const int s = 2 * tau;
    const int t = T;

    // 步骤 1：计算最小 Trek 滞后（get_minLag）。
    for (size_t si = 0; si < sources.size(); ++si) {
        const int src1 = sources[si];
        const int src = src1 - 1;

        for (size_t ti = 0; ti < targets.size(); ++ti) {
            const int dst1 = targets[ti];
            const int dst = dst1 - 1;
            if (src == dst) continue;

            for (int lag = 1; lag <= tau; ++lag) {
                std::vector<double> x;
                std::vector<double> y;
                std::vector<std::vector<double>> Z;

                const int len = t - s + 1;
                x.reserve((size_t)len);
                y.reserve((size_t)len);
                Z.reserve((size_t)len);

                for (int col = s; col <= t; ++col) {
                    x.push_back(dt[src][col - lag - 1]);
                    y.push_back(dt[dst][col - 1]);

                    std::vector<double> zrow;
                    zrow.reserve((size_t)tau);
                    zrow.push_back(dt[dst][col - 2]); // 目标变量在前一时刻的取值。

                    for (int pid = 1; pid <= tau - 1; ++pid) {
                        zrow.push_back(dt[src][col - lag - pid - 1]); // 原因变量更早时刻的 post 项。
                    }
                    Z.push_back(zrow);
                }

                if (pacot_dependent_reference(x, y, Z, alpha)) {
                    MinLagEdge e;
                    e.src = src1;
                    e.dst = dst1;
                    e.lag = lag;
                    min_lag_edges.push_back(e);
                    break;
                }
            }
        }
    }

    // 步骤 2：检测直接因果关系（get_causality）。
    for (size_t m = 0; m < min_lag_edges.size(); ++m) {
        const int xid = min_lag_edges[m].src;
        const int yid = min_lag_edges[m].dst;
        const int mlag = min_lag_edges[m].lag;
        const int xidx = xid - 1;
        const int yidx = yid - 1;

        std::vector<double> y;
        y.reserve((size_t)(t - s + 1));
        for (int col = s; col <= t; ++col) {
            y.push_back(dt[yidx][col - 1]);
        }

        // Z 由 y 的前一时刻，以及其他指向 y 的最小滞后父边展开得到的条件变量组成。
        std::vector<std::vector<double>> baseZ;
        baseZ.reserve((size_t)(t - s + 1));
        for (int col = s; col <= t; ++col) {
            std::vector<double> zrow;
            zrow.push_back(dt[yidx][col - 2]); // 初始化 Zy。
            baseZ.push_back(zrow);
        }

        for (size_t k = 0; k < min_lag_edges.size(); ++k) {
            if (k == m) continue;
            if (min_lag_edges[k].dst != yid) continue;

            const int zid = min_lag_edges[k].src;
            const int zlag = min_lag_edges[k].lag;
            const int zidx = zid - 1;

            for (int pid = 0; pid <= tau - zlag; ++pid) {
                for (int row = 0, col = s; col <= t; ++col, ++row) {
                    baseZ[row].push_back(dt[zidx][col - zlag - pid - 1]);
                }
            }
        }

        for (int lag = mlag; lag <= tau; ++lag) {
            std::vector<double> x;
            x.reserve((size_t)(t - s + 1));
            for (int col = s; col <= t; ++col) {
                x.push_back(dt[xidx][col - lag - 1]);
            }

            std::vector<std::vector<double>> Z = baseZ;

            // Zx 由 x 的 post 项和 front 项组成。
            for (int row = 0, col = s; col <= t; ++col, ++row) {
                Z[row].insert(Z[row].begin(), dt[xidx][col - lag - 2]); // post 项。
            }
            if (lag >= 2) {
                for (int qid = 1; qid <= lag - 1; ++qid) {
                    for (int row = 0, col = s; col <= t; ++col, ++row) {
                        Z[row].insert(Z[row].begin() + qid, dt[xidx][col - lag + qid - 1]); // front 项。
                    }
                }
            }

            if (pacot_dependent_reference(x, y, Z, alpha)) {
                out.lags_by_edge[std::make_pair(xid, yid)].push_back(lag);
            }
        }
    }

    // 去重并按滞后顺序排序。
    for (std::map<std::pair<int,int>, std::vector<int>>::iterator it = out.lags_by_edge.begin();
         it != out.lags_by_edge.end(); ++it) {
        std::vector<int>& vec = it->second;
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    }

    // 步骤 3：检测自因果关系（get_causality_self）。
    // 与参考 MATLAB 实现保持相同的判定思路：
    // 1）若节点不存在外部父变量，则直接加入滞后为 1 的自因果边；
    // 2）否则，在控制外部父变量后，检验 Xi(t-1) 与 Xi(t) 是否仍然相关。
    for (size_t idx = 0; idx < self_nodes.size(); ++idx) {
        const int node1 = self_nodes[idx];
        const int node = node1 - 1;

        std::vector<std::pair<int,int>> parents; // 二元组依次表示父节点编号和对应滞后。
        for (std::map<std::pair<int,int>, std::vector<int>>::const_iterator it = out.lags_by_edge.begin();
             it != out.lags_by_edge.end(); ++it) {
            if (it->first.second != node1) continue;
            if (it->first.first == node1) continue;
            for (size_t j = 0; j < it->second.size(); ++j) {
                parents.push_back(std::make_pair(it->first.first, it->second[j]));
            }
        }

        bool add_self = false;

        if (parents.empty()) {
            add_self = true;
        } else {
            std::vector<double> x;
            std::vector<double> y;
            std::vector<std::vector<double>> Z;

            int start_col = s;
            for (size_t k = 0; k < parents.size(); ++k) {
                start_col = std::max(start_col, parents[k].second + 1);
            }

            if (start_col <= t) {
                for (int col = start_col; col <= t; ++col) {
                    x.push_back(dt[node][col - 2]);
                    y.push_back(dt[node][col - 1]);

                    std::vector<double> zrow;
                    for (size_t k = 0; k < parents.size(); ++k) {
                        const int pidx = parents[k].first - 1;
                        const int plag = parents[k].second;
                        zrow.push_back(dt[pidx][col - plag - 1]);
                    }
                    Z.push_back(zrow);
                }
                add_self = pacot_dependent_reference(x, y, Z, alpha);
            }
        }

        if (add_self) {
            std::vector<int>& self_lags = out.lags_by_edge[std::make_pair(node1, node1)];
            if (std::find(self_lags.begin(), self_lags.end(), 1) == self_lags.end()) {
                self_lags.push_back(1);
                std::sort(self_lags.begin(), self_lags.end());
            }
        }
    }

    return out;
}

CDiTResult run_cdit_covmeta(const std::vector<std::vector<double>>& data,
                            int tau,
                            double alpha,
                            bool infer_self,
                            const std::vector<int>* source_columns,
                            const std::vector<int>* target_columns) {
    CDiTResult out;
    if (data.empty() || data[0].empty() || tau <= 0) return out;

    const int T = (int)data.size();
    const int d = (int)data[0].size();
    if (T < 2 * tau) return out;

    const std::vector<int> sources = normalize_column_list(source_columns, d);
    const std::vector<int> targets = normalize_column_list(target_columns, d);
    const std::vector<int> self_nodes = get_allowed_self_nodes(infer_self, sources, targets);

    const std::vector<std::vector<double>> dt = transpose_to_dim_time(data); // 矩阵按 [变量][时间] 组织。
    const LaggedCovCache cache = build_lagged_cov_cache(dt, T, d, tau);

    struct MinLagEdge {
        int src;
        int dst;
        int lag;
    };
    std::vector<MinLagEdge> min_lag_edges;

    // 步骤 1：计算最小 Trek 滞后。搜索顺序与原始实现保持一致，
    // 但每次条件独立性检验都直接从滞后协方差缓存中读取所需列。
    for (size_t si = 0; si < sources.size(); ++si) {
        const int src1 = sources[si];
        const int src = src1 - 1;

        for (size_t ti = 0; ti < targets.size(); ++ti) {
            const int dst1 = targets[ti];
            const int dst = dst1 - 1;
            if (src == dst) continue;

            for (int lag = 1; lag <= tau; ++lag) {
                const int x_col = cache.col_id(src, lag);
                const int y_col = cache.col_id(dst, 0);

                std::vector<int> z_cols;
                z_cols.reserve((size_t)tau);
                z_cols.push_back(cache.col_id(dst, 1));
                for (int pid = 1; pid <= tau - 1; ++pid) {
                    z_cols.push_back(cache.col_id(src, lag + pid));
                }

                if (pacot_dependent_cached(cache, x_col, y_col, z_cols, alpha)) {
                    MinLagEdge e;
                    e.src = src1;
                    e.dst = dst1;
                    e.lag = lag;
                    min_lag_edges.push_back(e);
                    break;
                }
            }
        }
    }

    // 步骤 2：检测直接因果关系。
    for (size_t m = 0; m < min_lag_edges.size(); ++m) {
        const int xid = min_lag_edges[m].src;
        const int yid = min_lag_edges[m].dst;
        const int mlag = min_lag_edges[m].lag;
        const int xidx = xid - 1;
        const int yidx = yid - 1;

        const int y_col = cache.col_id(yidx, 0);

        std::vector<int> baseZ;
        baseZ.push_back(cache.col_id(yidx, 1));

        for (size_t k = 0; k < min_lag_edges.size(); ++k) {
            if (k == m) continue;
            if (min_lag_edges[k].dst != yid) continue;

            const int zid = min_lag_edges[k].src;
            const int zlag = min_lag_edges[k].lag;
            const int zidx = zid - 1;

            for (int pid = 0; pid <= tau - zlag; ++pid) {
                baseZ.push_back(cache.col_id(zidx, zlag + pid));
            }
        }

        for (int lag = mlag; lag <= tau; ++lag) {
            const int x_col = cache.col_id(xidx, lag);

            std::vector<int> z_cols = baseZ;

            // Zx 的逻辑变量顺序与原始实现保持一致：
            // 先加入原因变量的 lag+1，再依次加入 lag-1、lag-2、……、1，最后加入 baseZ。
            z_cols.insert(z_cols.begin(), cache.col_id(xidx, lag + 1));
            for (int qid = 1; qid <= lag - 1; ++qid) {
                z_cols.insert(z_cols.begin() + qid, cache.col_id(xidx, lag - qid));
            }

            if (pacot_dependent_cached(cache, x_col, y_col, z_cols, alpha)) {
                out.lags_by_edge[std::make_pair(xid, yid)].push_back(lag);
            }
        }
    }

    for (std::map<std::pair<int,int>, std::vector<int>>::iterator it = out.lags_by_edge.begin();
         it != out.lags_by_edge.end(); ++it) {
        std::vector<int>& vec = it->second;
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    }

    // 步骤 3：检测自因果关系，判定语义与原始实现保持一致。
    for (size_t idx = 0; idx < self_nodes.size(); ++idx) {
        const int node1 = self_nodes[idx];
        const int node = node1 - 1;

        std::vector<std::pair<int,int>> parents; // 二元组依次表示父节点编号和对应滞后。
        for (std::map<std::pair<int,int>, std::vector<int>>::const_iterator it = out.lags_by_edge.begin();
             it != out.lags_by_edge.end(); ++it) {
            if (it->first.second != node1) continue;
            if (it->first.first == node1) continue;
            for (size_t j = 0; j < it->second.size(); ++j) {
                parents.push_back(std::make_pair(it->first.first, it->second[j]));
            }
        }

        bool add_self = false;

        if (parents.empty()) {
            add_self = true;
        } else {
            const int x_col = cache.col_id(node, 1);
            const int y_col = cache.col_id(node, 0);
            std::vector<int> z_cols;
            z_cols.reserve(parents.size());
            for (size_t k = 0; k < parents.size(); ++k) {
                const int pidx = parents[k].first - 1;
                const int plag = parents[k].second;
                z_cols.push_back(cache.col_id(pidx, plag));
            }
            add_self = pacot_dependent_cached(cache, x_col, y_col, z_cols, alpha);
        }

        if (add_self) {
            std::vector<int>& self_lags = out.lags_by_edge[std::make_pair(node1, node1)];
            if (std::find(self_lags.begin(), self_lags.end(), 1) == self_lags.end()) {
                self_lags.push_back(1);
                std::sort(self_lags.begin(), self_lags.end());
            }
        }
    }

    return out;
}

CDiTResult run_cdit(const std::vector<std::vector<double>>& data,
                    int tau,
                    double alpha,
                    bool infer_self,
                    const std::vector<int>* source_columns,
                    const std::vector<int>* target_columns) {
    return run_cdit_original(data, tau, alpha, infer_self, source_columns, target_columns);
}

std::string format_inferred(const CDiTResult& r) {
    std::ostringstream oss;
    for (std::map<std::pair<int,int>, std::vector<int>>::const_iterator it = r.lags_by_edge.begin();
         it != r.lags_by_edge.end(); ++it) {
        if (it->second.empty()) continue;
        oss << it->first.first << "->" << it->first.second << "滞后=";
        std::vector<int> sorted = it->second;
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 0; i < sorted.size(); ++i) oss << sorted[i] << ",";
        oss << "; ";
    }
    return oss.str();
}

std::string format_inferred_named(const CDiTResult& r,
                                  const std::vector<std::string>& names) {
    struct NameGetter {
        static std::string get(int idx, const std::vector<std::string>& names) {
            if (idx >= 1 && idx <= (int)names.size() && !names[idx - 1].empty()) return names[idx - 1];
            std::ostringstream oss;
            oss << 'X' << idx;
            return oss.str();
        }
    };

    std::ostringstream oss;
    for (std::map<std::pair<int,int>, std::vector<int>>::const_iterator it = r.lags_by_edge.begin();
         it != r.lags_by_edge.end(); ++it) {
        if (it->second.empty()) continue;
        oss << NameGetter::get(it->first.first, names)
            << "→"
            << NameGetter::get(it->first.second, names)
            << "滞后=";
        std::vector<int> sorted = it->second;
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 0; i < sorted.size(); ++i) oss << sorted[i] << ",";
        oss << "; ";
    }
    return oss.str();
}

std::string format_inferred_pretty(const CDiTResult& r) {
    std::ostringstream oss;
    for (std::map<std::pair<int,int>, std::vector<int>>::const_iterator it = r.lags_by_edge.begin();
         it != r.lags_by_edge.end(); ++it) {
        if (it->second.empty()) continue;
        oss << "X" << it->first.first << "→X" << it->first.second << "滞后=";
        std::vector<int> sorted = it->second;
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 0; i < sorted.size(); ++i) oss << sorted[i] << ",";
        oss << "; ";
    }
    return oss.str();
}

std::string format_abstract_graph(const CDiTResult& r) {
    std::ostringstream oss;
    bool first = true;
    for (std::map<std::pair<int,int>, std::vector<int>>::const_iterator it = r.lags_by_edge.begin();
         it != r.lags_by_edge.end(); ++it) {
        if (it->second.empty()) continue;
        if (!first) oss << ' ';
        first = false;
        oss << "X" << it->first.first << "→X" << it->first.second;
    }
    return oss.str();
}
