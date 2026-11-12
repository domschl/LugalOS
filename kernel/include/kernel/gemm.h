/* kernel/include/kernel/gemm.h — Multicore INT8 GEMM Engine
 *
 * Implements high-throughput integer General Matrix Multiplication (GEMM)
 * and quantized neural network linear layers for RISC-V 64-bit SMP platforms
 * (StarFive VisionFive 2 JH7110 4x SiFive U74 cores).
 *
 * Zero floating-point arithmetic (pure integer fixed-point math).
 */

#ifndef LUGALOS_KERNEL_GEMM_H
#define LUGALOS_KERNEL_GEMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Initialize multicore GEMM engine and persistent worker tasks */
bool gemm_init(void);

/* Single-core reference INT8 GEMM: C[M x N] = A[M x K] * B^T[N x K] */
void gemm_int8_ref(const int8_t *a, const int8_t *b_trans, int32_t *c, int M, int N, int K);

/* Multicore INT8 GEMM: C[M x N] = A[M x K] * B^T[N x K]
 * num_cores: 1, 2, or 4 (or <= 0 for all online harts) */
void gemm_int8_multicore(const int8_t *a, const int8_t *b_trans, int32_t *c,
                         int M, int N, int K, int num_cores);

/* Quantized linear layer with bias and fixed-point scale:
 * out[M x N] = clamp_int8((A[M x K] * B^T[N x K] + bias[N]) >> shift) */
void gemm_int8_quantized(const int8_t *a, const int8_t *b_trans, const int32_t *bias,
                         int8_t *out, int M, int N, int K, int shift, int num_cores);

/* Run benchmark comparing 1, 2, 4 cores and verifying numerical accuracy */
void gemm_benchmark(int M, int N, int K, int iterations);

/* Shell command entry point */
void gemm_cmd(const char *arg);

#endif /* LUGALOS_KERNEL_GEMM_H */
