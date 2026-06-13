#ifndef VESTA_BLAS_DEBUG
#  define VESTA_BLAS_DEBUG 0
#endif

#include "../../../include/ffi/vesta_plugin.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <float.h>

#define VESTA_CAP_FFI_CALL  (1u << 3)
#define VBLAS_EPS    1.0e-15
#define VBLAS_MAX_IT 200

static const VestaPluginAPI *g_api = NULL;

static inline double u64_to_f64(uint64_t u) {
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static inline uint64_t f64_to_u64(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static double *vblas_read_doubles(uint64_t proc_ptr, uint64_t vm_addr, uint64_t n) {
    if (!g_api || n == 0) return NULL;
    double *buf = (double *)malloc((size_t)(n * sizeof(double)));
    if (!buf) return NULL;
    g_api->vm_read_bytes(proc_ptr, vm_addr, buf, n * sizeof(double));
    return buf;
}

static int vblas_write_doubles(uint64_t proc_ptr, uint64_t vm_addr, const double *src, uint64_t n) {
    if (!g_api || n == 0) return 0;
    return (int)g_api->vm_write_bytes(proc_ptr, vm_addr, src, n * sizeof(double));
}

VESTA_PLUGIN_EXPORT void vesta_init(const VestaPluginAPI *api) {
    g_api = api;
#if VESTA_BLAS_DEBUG
    if (api) api->log("[vesta_blas] loaded");
#else
    (void) api;
#endif
}

static void vblas_dot_impl(int n, const double *x, const double *y, double *result) {
    double s = 0.0;
    int i;
    for (i = 0; i < n; i++) s += x[i] * y[i];
    *result = s;
}

static void vblas_axpy_impl(int n, double a, const double *x, double *y) {
    int i;
    for (i = 0; i < n; i++) y[i] = a * x[i] + y[i];
}

static void vblas_nrm2_impl(int n, const double *x, double *result) {
    double s = 0.0;
    int i;
    for (i = 0; i < n; i++) s += x[i] * x[i];
    *result = sqrt(s);
}

static void vblas_asum_impl(int n, const double *x, double *result) {
    double s = 0.0;
    int i;
    for (i = 0; i < n; i++) s += fabs(x[i]);
    *result = s;
}

static void vblas_iamax_impl(int n, const double *x, int *result) {
    if (n <= 0) { *result = -1; return; }
    int idx = 0;
    double maxv = fabs(x[0]);
    int i;
    for (i = 1; i < n; i++) {
        double v = fabs(x[i]);
        if (v > maxv) { maxv = v; idx = i; }
    }
    *result = idx;
}

static void vblas_scal_impl(int n, double a, double *x) {
    int i;
    for (i = 0; i < n; i++) x[i] *= a;
}

static void vblas_copy_impl(int n, const double *x, double *y) {
    memcpy(y, x, (size_t)n * sizeof(double));
}

static void vblas_swap_impl(int n, double *x, double *y) {
    int i;
    for (i = 0; i < n; i++) {
        double t = x[i]; x[i] = y[i]; y[i] = t;
    }
}

/* Level 1 */

VESTA_PLUGIN_EXPORT uint64_t vblas_dot(uint64_t proc_ptr, uint64_t x_addr, uint64_t y_addr, uint64_t n) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return f64_to_u64(0.0);
    double *x = vblas_read_doubles(proc_ptr, x_addr, n);
    double *y = vblas_read_doubles(proc_ptr, y_addr, n);
    if (!x || !y) { free(x); free(y); return f64_to_u64(0.0); }
    double result;
    vblas_dot_impl((int)n, x, y, &result);
    free(x); free(y);
    return f64_to_u64(result);
}

VESTA_PLUGIN_EXPORT uint64_t vblas_axpy(uint64_t proc_ptr, uint64_t a_bits, uint64_t x_addr, uint64_t y_addr, uint64_t n) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 0;
    double a = u64_to_f64(a_bits);
    double *x = vblas_read_doubles(proc_ptr, x_addr, n);
    double *y = vblas_read_doubles(proc_ptr, y_addr, n);
    if (!x || !y) { free(x); free(y); return 0; }
    vblas_axpy_impl((int)n, a, x, y);
    vblas_write_doubles(proc_ptr, y_addr, y, n);
    free(x); free(y);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vblas_nrm2(uint64_t proc_ptr, uint64_t x_addr, uint64_t n) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return f64_to_u64(0.0);
    double *x = vblas_read_doubles(proc_ptr, x_addr, n);
    if (!x) return f64_to_u64(0.0);
    double result;
    vblas_nrm2_impl((int)n, x, &result);
    free(x);
    return f64_to_u64(result);
}

VESTA_PLUGIN_EXPORT uint64_t vblas_asum(uint64_t proc_ptr, uint64_t x_addr, uint64_t n) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return f64_to_u64(0.0);
    double *x = vblas_read_doubles(proc_ptr, x_addr, n);
    if (!x) return f64_to_u64(0.0);
    double result;
    vblas_asum_impl((int)n, x, &result);
    free(x);
    return f64_to_u64(result);
}

VESTA_PLUGIN_EXPORT uint64_t vblas_iamax(uint64_t proc_ptr, uint64_t x_addr, uint64_t n) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 0;
    double *x = vblas_read_doubles(proc_ptr, x_addr, n);
    if (!x) return 0;
    int result;
    vblas_iamax_impl((int)n, x, &result);
    free(x);
    return (uint64_t)result;
}

VESTA_PLUGIN_EXPORT uint64_t vblas_scal(uint64_t proc_ptr, uint64_t a_bits, uint64_t x_addr, uint64_t n) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 0;
    double a = u64_to_f64(a_bits);
    double *x = vblas_read_doubles(proc_ptr, x_addr, n);
    if (!x) return 0;
    vblas_scal_impl((int)n, a, x);
    vblas_write_doubles(proc_ptr, x_addr, x, n);
    free(x);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vblas_copy(uint64_t proc_ptr, uint64_t x_addr, uint64_t y_addr, uint64_t n) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 0;
    double *x = vblas_read_doubles(proc_ptr, x_addr, n);
    if (!x) return 0;
    vblas_write_doubles(proc_ptr, y_addr, x, n);
    free(x);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vblas_swap(uint64_t proc_ptr, uint64_t x_addr, uint64_t y_addr, uint64_t n) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 0;
    double *x = vblas_read_doubles(proc_ptr, x_addr, n);
    double *y = vblas_read_doubles(proc_ptr, y_addr, n);
    if (!x || !y) { free(x); free(y); return 0; }
    vblas_swap_impl((int)n, x, y);
    vblas_write_doubles(proc_ptr, x_addr, x, n);
    vblas_write_doubles(proc_ptr, y_addr, y, n);
    free(x); free(y);
    return 0;
}

/* Level 2: GEMV */

static void vblas_gemv_impl(int m, int n, double alpha, const double *a, const double *x, double beta, double *y) {
    int i, j;
    for (i = 0; i < m; i++) {
        double sum = 0.0;
        for (j = 0; j < n; j++) sum += a[(size_t)i * n + j] * x[j];
        y[i] = alpha * sum + beta * y[i];
    }
}

VESTA_PLUGIN_EXPORT uint64_t vblas_gemv(uint64_t proc_ptr, uint64_t m, uint64_t n, uint64_t alpha_bits,
                                        uint64_t a_addr, uint64_t x_addr, uint64_t beta_bits, uint64_t y_addr) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 0;
    double alpha = u64_to_f64(alpha_bits);
    double beta  = u64_to_f64(beta_bits);
    int mm = (int)m, nn = (int)n;
    double *a = vblas_read_doubles(proc_ptr, a_addr, (uint64_t)(mm * nn));
    double *x = vblas_read_doubles(proc_ptr, x_addr, (uint64_t)nn);
    double *y = vblas_read_doubles(proc_ptr, y_addr, (uint64_t)mm);
    if (!a || !x || !y) { free(a); free(x); free(y); return 0; }
    vblas_gemv_impl(mm, nn, alpha, a, x, beta, y);
    vblas_write_doubles(proc_ptr, y_addr, y, (uint64_t)mm);
    free(a); free(x); free(y);
    return 0;
}

/* Level 3: GEMM */

static void vblas_gemm_impl(int m, int n, int k, double alpha, const double *a, const double *b, double beta, double *c) {
    int i, j, p;
    for (i = 0; i < m; i++) {
        for (j = 0; j < n; j++) {
            double sum = 0.0;
            for (p = 0; p < k; p++)
                sum += a[(size_t)i * k + p] * b[(size_t)p * n + j];
            c[(size_t)i * n + j] = alpha * sum + beta * c[(size_t)i * n + j];
        }
    }
}

VESTA_PLUGIN_EXPORT uint64_t vblas_gemm(uint64_t proc_ptr, uint64_t m, uint64_t n, uint64_t k,
                                        uint64_t alpha_bits, uint64_t a_addr, uint64_t b_addr,
                                        uint64_t beta_bits, uint64_t c_addr) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 0;
    double alpha = u64_to_f64(alpha_bits);
    double beta  = u64_to_f64(beta_bits);
    int mm = (int)m, nn = (int)n, kk = (int)k;
    double *a = vblas_read_doubles(proc_ptr, a_addr, (uint64_t)(mm * kk));
    double *b = vblas_read_doubles(proc_ptr, b_addr, (uint64_t)(kk * nn));
    double *c = vblas_read_doubles(proc_ptr, c_addr, (uint64_t)(mm * nn));
    if (!a || !b || !c) { free(a); free(b); free(c); return 0; }
    vblas_gemm_impl(mm, nn, kk, alpha, a, b, beta, c);
    vblas_write_doubles(proc_ptr, c_addr, c, (uint64_t)(mm * nn));
    free(a); free(b); free(c);
    return 0;
}

/* QR decomposition - Householder reflections */

static void qr_householder(int m, int n, const double *a, double *q, double *r) {
    int i, j, k;
    double *a_copy = (double *)malloc((size_t)(m * n) * sizeof(double));
    if (!a_copy) return;
    memcpy(a_copy, a, (size_t)(m * n) * sizeof(double));

    for (i = 0; i < m; i++) for (j = 0; j < m; j++) q[i * m + j] = (i == j) ? 1.0 : 0.0;

    for (k = 0; k < n && k < m; k++) {
        double norm = 0.0;
        for (i = k; i < m; i++) norm += a_copy[i * n + k] * a_copy[i * n + k];
        norm = sqrt(norm);
        if (norm == 0.0) continue;

        double sign = (a_copy[k * n + k] >= 0) ? 1.0 : -1.0;
        double u1 = a_copy[k * n + k] + sign * norm;
        for (i = k + 1; i < m; i++) a_copy[i * n + k] /= u1;
        a_copy[k * n + k] = 1.0;

        for (j = k + 1; j < n; j++) {
            double dot = 0.0;
            for (i = k; i < m; i++) dot += a_copy[i * n + k] * a_copy[i * n + j];
            for (i = k; i < m; i++) a_copy[i * n + j] -= 2.0 * a_copy[i * n + k] * dot;
        }
        a_copy[k * n + k] = -sign * norm;

        for (i = 0; i < m; i++) {
            double dot = 0.0;
            for (int p = k; p < m; p++) dot += q[i * m + p] * a_copy[p * n + k];
            for (int p = k; p < m; p++) q[i * m + p] -= 2.0 * dot * a_copy[p * n + k];
        }
    }

    for (i = 0; i < m; i++) for (j = 0; j < n; j++) r[i * n + j] = 0.0;
    for (i = 0; i < m; i++) for (j = i; j < n; j++) {
        double sum = 0.0;
        for (k = 0; k < m; k++) sum += q[k * m + i] * a[k * n + j];
        r[i * n + j] = sum;
    }

    free(a_copy);
}

VESTA_PLUGIN_EXPORT uint64_t vblas_qr(uint64_t proc_ptr, uint64_t m, uint64_t n,
                                      uint64_t a_addr, uint64_t q_addr, uint64_t r_addr) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 1;
    int mm = (int)m, nn = (int)n;
    double *a = vblas_read_doubles(proc_ptr, a_addr, (uint64_t)(mm * nn));
    if (!a) return 1;
    double *q = (double *)calloc((size_t)(mm * mm), sizeof(double));
    double *r = (double *)calloc((size_t)(mm * nn), sizeof(double));
    if (!q || !r) { free(a); free(q); free(r); return 1; }
    qr_householder(mm, nn, a, q, r);
    vblas_write_doubles(proc_ptr, q_addr, q, (uint64_t)(mm * mm));
    vblas_write_doubles(proc_ptr, r_addr, r, (uint64_t)(mm * nn));
    free(a); free(q); free(r);
    return 0;
}

/* Cholesky decomposition A = L * L^T */

static int cholesky_decomp(int n, const double *a, double *l) {
    int i, j, k;
    memset(l, 0, (size_t)(n * n) * sizeof(double));
    for (j = 0; j < n; j++) {
        double sum = 0.0;
        for (k = 0; k < j; k++) sum += l[j * n + k] * l[j * n + k];
        double val = a[j * n + j] - sum;
        if (val <= 0.0) return 1;
        l[j * n + j] = sqrt(val);
        for (i = j + 1; i < n; i++) {
            sum = 0.0;
            for (k = 0; k < j; k++) sum += l[i * n + k] * l[j * n + k];
            l[i * n + j] = (a[i * n + j] - sum) / l[j * n + j];
        }
    }
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vblas_cholesky(uint64_t proc_ptr, uint64_t n,
                                            uint64_t a_addr, uint64_t l_addr) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 1;
    int nn = (int)n;
    double *a = vblas_read_doubles(proc_ptr, a_addr, (uint64_t)(nn * nn));
    if (!a) return 1;
    double *l = (double *)calloc((size_t)(nn * nn), sizeof(double));
    if (!l) { free(a); return 1; }
    int status = cholesky_decomp(nn, a, l);
    if (status == 0)
        vblas_write_doubles(proc_ptr, l_addr, l, (uint64_t)(nn * nn));
    free(a); free(l);
    return (uint64_t)status;
}

/* Symmetric Eigenvalue decomposition via Jacobi */

static int eig_jacobi(int n, const double *a, double *eigenvalues, double *eigenvectors) {
    int i, j, p, q, it;
    double *v = (double *)malloc((size_t)(n * n) * sizeof(double));
    double *d = (double *)malloc((size_t)n * sizeof(double));
    double *b = (double *)malloc((size_t)n * sizeof(double));
    double *z = (double *)malloc((size_t)n * sizeof(double));
    double *aa = (double *)malloc((size_t)(n * n) * sizeof(double));
    if (!v || !d || !b || !z || !aa) { free(v); free(d); free(b); free(z); free(aa); return 1; }
    memcpy(aa, a, (size_t)(n * n) * sizeof(double));

    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) v[i * n + j] = (i == j) ? 1.0 : 0.0;
        d[i] = aa[i * n + i];
        b[i] = d[i];
        z[i] = 0.0;
    }

    for (it = 0; it < VBLAS_MAX_IT; it++) {
        double sm = 0.0;
        for (p = 0; p < n - 1; p++)
            for (q = p + 1; q < n; q++)
                sm += fabs(aa[p * n + q]);
        if (sm == 0.0) break;

        double tresh = (it < 3) ? 0.2 * sm / (double)(n * n) : 0.0;
        for (p = 0; p < n - 1; p++) {
            for (q = p + 1; q < n; q++) {
                double g = 100.0 * fabs(aa[p * n + q]);
                if (it > 3 && fabs(d[p]) + g == fabs(d[p]) && fabs(d[q]) + g == fabs(d[q])) {
                    aa[p * n + q] = 0.0;
                } else if (fabs(aa[p * n + q]) > tresh) {
                    double h = d[q] - d[p];
                    double t;
                    if (fabs(h) + g == fabs(h)) {
                        t = aa[p * n + q] / h;
                    } else {
                        double theta = 0.5 * h / aa[p * n + q];
                        t = 1.0 / (fabs(theta) + sqrt(1.0 + theta * theta));
                        if (theta < 0.0) t = -t;
                    }
                    double c = 1.0 / sqrt(1.0 + t * t);
                    double s = t * c;
                    double tau = s / (1.0 + c);
                    h = t * aa[p * n + q];
                    z[p] -= h;
                    z[q] += h;
                    d[p] -= h;
                    d[q] += h;
                    aa[p * n + q] = 0.0;
                    for (j = 0; j < p; j++) { g = aa[j * n + p]; h = aa[j * n + q]; aa[j * n + p] = g - s * (h + g * tau); aa[j * n + q] = h + s * (g - h * tau); }
                    for (j = p + 1; j < q; j++) { g = aa[p * n + j]; h = aa[j * n + q]; aa[p * n + j] = g - s * (h + g * tau); aa[j * n + q] = h + s * (g - h * tau); }
                    for (j = q + 1; j < n; j++) { g = aa[p * n + j]; h = aa[q * n + j]; aa[p * n + j] = g - s * (h + g * tau); aa[q * n + j] = h + s * (g - h * tau); }
                    for (j = 0; j < n; j++) { g = v[j * n + p]; h = v[j * n + q]; v[j * n + p] = g - s * (h + g * tau); v[j * n + q] = h + s * (g - h * tau); }
                }
            }
        }
        for (i = 0; i < n; i++) { b[i] += z[i]; d[i] = b[i]; z[i] = 0.0; }
    }

    for (i = 0; i < n; i++) eigenvalues[i] = d[i];
    for (i = 0; i < n; i++) for (j = 0; j < n; j++) eigenvectors[i * n + j] = v[i * n + j];

    free(v); free(d); free(b); free(z); free(aa);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vblas_eig(uint64_t proc_ptr, uint64_t n,
                                       uint64_t a_addr, uint64_t eigenvalues_addr, uint64_t eigenvectors_addr) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 1;
    int nn = (int)n;
    double *a = vblas_read_doubles(proc_ptr, a_addr, (uint64_t)(nn * nn));
    if (!a) return 1;
    double *eigenvalues = (double *)malloc((size_t)nn * sizeof(double));
    double *eigenvectors = (double *)calloc((size_t)(nn * nn), sizeof(double));
    if (!eigenvalues || !eigenvectors) { free(a); free(eigenvalues); free(eigenvectors); return 1; }
    int status = eig_jacobi(nn, a, eigenvalues, eigenvectors);
    if (status == 0) {
        vblas_write_doubles(proc_ptr, eigenvalues_addr, eigenvalues, (uint64_t)nn);
        vblas_write_doubles(proc_ptr, eigenvectors_addr, eigenvectors, (uint64_t)(nn * nn));
    }
    free(a); free(eigenvalues); free(eigenvectors);
    return (uint64_t)status;
}

/* SVD via one-sided Jacobi */

static int svd_one_sided_jacobi(int m, int n, const double *a, double *u, double *s, double *vt) {
    int i, j, p, q, iter;
    double *v = (double *)malloc((size_t)(n * n) * sizeof(double));
    double *b = (double *)malloc((size_t)(m * n) * sizeof(double));
    if (!v || !b) { free(v); free(b); return 1; }

    memcpy(b, a, (size_t)(m * n) * sizeof(double));
    for (i = 0; i < n; i++) for (j = 0; j < n; j++) v[i * n + j] = (i == j) ? 1.0 : 0.0;

    int changed;
    for (iter = 0; iter < VBLAS_MAX_IT; iter++) {
        changed = 0;
        for (p = 0; p < n - 1; p++) {
            for (q = p + 1; q < n; q++) {
                double dot_pq = 0.0, norm_p = 0.0, norm_q = 0.0;
                for (i = 0; i < m; i++) {
                    double bip = b[i * n + p];
                    double biq = b[i * n + q];
                    dot_pq += bip * biq;
                    norm_p += bip * bip;
                    norm_q += biq * biq;
                }

                if (fabs(dot_pq) < VBLAS_EPS * sqrt(norm_p * norm_q)) continue;

                double theta = (norm_q - norm_p) / (2.0 * dot_pq);
                double t = 1.0 / (fabs(theta) + sqrt(1.0 + theta * theta));
                if (theta < 0.0) t = -t;
                double c = 1.0 / sqrt(1.0 + t * t);
                double ss = t * c;

                for (i = 0; i < m; i++) {
                    double bip = b[i * n + p];
                    double biq = b[i * n + q];
                    b[i * n + p] = c * bip + ss * biq;
                    b[i * n + q] = -ss * bip + c * biq;
                }
                for (i = 0; i < n; i++) {
                    double vip = v[i * n + p];
                    double viq = v[i * n + q];
                    v[i * n + p] = c * vip + ss * viq;
                    v[i * n + q] = -ss * vip + c * viq;
                }
                changed = 1;
            }
        }
        if (!changed) break;
    }

    for (j = 0; j < n; j++) {
        double norm = 0.0;
        for (i = 0; i < m; i++) norm += b[i * n + j] * b[i * n + j];
        s[j] = sqrt(norm);
        if (s[j] > 0.0) {
            for (i = 0; i < m; i++) b[i * n + j] /= s[j];
        }
    }

    for (i = 0; i < m; i++) for (j = 0; j < n; j++) u[i * n + j] = b[i * n + j];

    for (i = 0; i < n; i++) for (j = 0; j < n; j++) vt[i * n + j] = v[j * n + i];

    free(v); free(b);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vblas_svd(uint64_t proc_ptr, uint64_t m, uint64_t n,
                                       uint64_t a_addr, uint64_t u_addr, uint64_t s_addr, uint64_t vt_addr) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 1;
    int mm = (int)m, nn = (int)n;
    double *a = vblas_read_doubles(proc_ptr, a_addr, (uint64_t)(mm * nn));
    if (!a) return 1;
    double *u = (double *)calloc((size_t)(mm * nn), sizeof(double));
    double *sv = (double *)calloc((size_t)nn, sizeof(double));
    double *vt = (double *)calloc((size_t)(nn * nn), sizeof(double));
    if (!u || !sv || !vt) { free(a); free(u); free(sv); free(vt); return 1; }
    int status = svd_one_sided_jacobi(mm, nn, a, u, sv, vt);
    if (status == 0) {
        vblas_write_doubles(proc_ptr, u_addr, u, (uint64_t)(mm * nn));
        vblas_write_doubles(proc_ptr, s_addr, sv, (uint64_t)nn);
        vblas_write_doubles(proc_ptr, vt_addr, vt, (uint64_t)(nn * nn));
    }
    free(a); free(u); free(sv); free(vt);
    return (uint64_t)status;
}

/* Convolution 2D for neural networks */

static void vblas_conv2d_impl(int in_c, int in_h, int in_w, int out_c,
                              int k_h, int k_w, int stride, int pad,
                              const double *input, const double *kernel,
                              double *output) {
    int out_h = (in_h + 2 * pad - k_h) / stride + 1;
    int out_w = (in_w + 2 * pad - k_w) / stride + 1;
    int oc, ic, i, j, kh, kw;
    for (oc = 0; oc < out_c; oc++) {
        for (i = 0; i < out_h; i++) {
            for (j = 0; j < out_w; j++) {
                double sum = 0.0;
                for (ic = 0; ic < in_c; ic++) {
                    for (kh = 0; kh < k_h; kh++) {
                        for (kw = 0; kw < k_w; kw++) {
                            int ih = i * stride + kh - pad;
                            int iw = j * stride + kw - pad;
                            if (ih >= 0 && ih < in_h && iw >= 0 && iw < in_w) {
                                sum += input[ic * in_h * in_w + ih * in_w + iw]
                                     * kernel[oc * in_c * k_h * k_w + ic * k_h * k_w + kh * k_w + kw];
                            }
                        }
                    }
                }
                output[oc * out_h * out_w + i * out_w + j] = sum;
            }
        }
    }
}

VESTA_PLUGIN_EXPORT uint64_t vblas_conv2d(uint64_t proc_ptr, uint64_t in_c, uint64_t in_h, uint64_t in_w,
                                          uint64_t out_c, uint64_t k_h, uint64_t k_w,
                                          uint64_t stride, uint64_t pad,
                                          uint64_t input_addr, uint64_t kernel_addr, uint64_t output_addr) {
    if (g_api && g_api->cap_check && !g_api->cap_check(VESTA_CAP_FFI_CALL)) return 1;
    int inc = (int)in_c, inh = (int)in_h, inw = (int)in_w;
    int outc = (int)out_c, kh = (int)k_h, kw = (int)k_w;
    int st = (int)stride, pd = (int)pad;
    int outh = (inh + 2 * pd - kh) / st + 1;
    int outw = (inw + 2 * pd - kw) / st + 1;

    double *input = vblas_read_doubles(proc_ptr, input_addr, (uint64_t)(inc * inh * inw));
    double *kernel = vblas_read_doubles(proc_ptr, kernel_addr, (uint64_t)(outc * inc * kh * kw));
    double *output = (double *)calloc((size_t)(outc * outh * outw), sizeof(double));
    if (!input || !kernel || !output) { free(input); free(kernel); free(output); return 1; }

    vblas_conv2d_impl(inc, inh, inw, outc, kh, kw, st, pd, input, kernel, output);
    vblas_write_doubles(proc_ptr, output_addr, output, (uint64_t)(outc * outh * outw));

    free(input); free(kernel); free(output);
    return 0;
}
