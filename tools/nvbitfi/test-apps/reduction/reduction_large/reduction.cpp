/* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
    Parallel reduction

    This sample shows how to perform a reduction operation on an array of values
    to produce a single value.

    Reductions are a very common computation in parallel algorithms.  Any time
    an array of values needs to be reduced to a single value using a binary
    associative operator, a reduction can be used.  Example applications include
    statistics computations such as mean and standard deviation, and image
    processing applications such as finding the total luminance of an
    image.

    This code performs sum reductions, but any associative operator such as
    min() or max() could also be used.

    It assumes the input size is a power of 2.

    COMMAND LINE ARGUMENTS

    "--shmoo":         Test performance for 1 to 32M elements with each of the 7
   different kernels
    "--n=<N>":         Specify the number of elements to reduce (default
   1048576)
    "--threads=<N>":   Specify the number of threads per block (default 128)
    "--kernel=<N>":    Specify which kernel to run (0-6, default 6)
    "--maxblocks=<N>": Specify the maximum number of thread blocks to launch
   (kernel 6 only, default 64)
    "--cpufinal":      Read back the per-block results and do final sum of block
   sums on CPU (default false)
    "--cputhresh=<N>": The threshold of number of blocks sums below which to
   perform a CPU final reduction (default 1)
    "-type=<T>":       The datatype for the reduction, where T is "int",
   "float", or "double" (default int)
*/

// CUDA Runtime
#include <cuda_runtime.h>

// Utilities and system includes
#include <helper_cuda.h>
#include <helper_functions.h>
#include <algorithm>

// includes, project
#include "reduction.h"

enum ReduceType { REDUCE_INT, REDUCE_FLOAT, REDUCE_DOUBLE };

////////////////////////////////////////////////////////////////////////////////
// declaration, forward
template <class T>
bool runTest(int argc, char **argv, ReduceType datatype);

#define MAX_BLOCK_DIM_SIZE 65535

#ifdef WIN32
#define strcasecmp strcmpi
#endif

extern "C" bool isPow2(unsigned int x) { return ((x & (x - 1)) == 0); }

const char *getReduceTypeString(const ReduceType type) {
  switch (type) {
    case REDUCE_INT:
      return "int";
    case REDUCE_FLOAT:
      return "float";
    case REDUCE_DOUBLE:
      return "double";
    default:
      return "unknown";
  }
}

////////////////////////////////////////////////////////////////////////////////
// Program main
////////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv) {
  // printf("%s Starting...\n\n", argv[0]);

  char *typeInput = 0;
  getCmdLineArgumentString(argc, (const char **)argv, "type", &typeInput);

  ReduceType datatype = REDUCE_INT;

  if (0 != typeInput) {
    if (!strcasecmp(typeInput, "float")) {
      datatype = REDUCE_FLOAT;
    } else if (!strcasecmp(typeInput, "double")) {
      datatype = REDUCE_DOUBLE;
    } else if (strcasecmp(typeInput, "int")) {
      printf("Type %s is not recognized. Using default type int.\n\n",
             typeInput);
    }
  }

  cudaDeviceProp deviceProp;
  int dev;

  dev = findCudaDevice(argc, (const char **)argv);

  checkCudaErrors(cudaGetDeviceProperties(&deviceProp, dev));

  printf("Using Device %d: %s\n\n", dev, deviceProp.name);
  checkCudaErrors(cudaSetDevice(dev));

  printf("Reducing array of type %s\n\n", getReduceTypeString(datatype));

  bool bResult = false;

  switch (datatype) {
    default:
    case REDUCE_INT:
      bResult = runTest<int>(argc, argv, datatype);
      break;

    case REDUCE_FLOAT:
      bResult = runTest<float>(argc, argv, datatype);
      break;

    case REDUCE_DOUBLE:
      bResult = runTest<double>(argc, argv, datatype);
      break;
  }

  printf(bResult ? "Test passed\n" : "Test failed!\n");
}

////////////////////////////////////////////////////////////////////////////////
//! Compute sum reduction on CPU
//! We use Kahan summation for an accurate sum of large arrays.
//! http://en.wikipedia.org/wiki/Kahan_summation_algorithm
//!
//! @param data       pointer to input data
//! @param size       number of input data elements
////////////////////////////////////////////////////////////////////////////////
template <class T>
T reduceCPU(T *data, int size) {
  T sum = data[0];
  T c = (T)0.0;

  for (int i = 1; i < size; i++) {
    T y = data[i] - c;
    T t = sum + y;
    c = (t - sum) - y;
    sum = t;
  }

  return sum;
}

unsigned int nextPow2(unsigned int x) {
  --x;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  return ++x;
}

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

////////////////////////////////////////////////////////////////////////////////
// Compute the number of threads and blocks to use for the given reduction
// kernel For the kernels >= 3, we set threads / block to the minimum of
// maxThreads and n/2. For kernels < 3, we set to the minimum of maxThreads and
// n.  For kernel 6, we observe the maximum specified number of blocks, because
// each thread in that kernel can process a variable number of elements.
////////////////////////////////////////////////////////////////////////////////
void getNumBlocksAndThreads(int whichKernel, int n, int maxBlocks,
                            int maxThreads, int &blocks, int &threads) {
  // get device capability, to avoid block/grid size exceed the upper bound
  cudaDeviceProp prop;
  int device;
  checkCudaErrors(cudaGetDevice(&device));
  checkCudaErrors(cudaGetDeviceProperties(&prop, device));

  if (whichKernel < 3) {
    threads = (n < maxThreads) ? nextPow2(n) : maxThreads;
    blocks = (n + threads - 1) / threads;
  } else {
    threads = (n < maxThreads * 2) ? nextPow2((n + 1) / 2) : maxThreads;
    blocks = (n + (threads * 2 - 1)) / (threads * 2);
  }

  if ((float)threads * blocks >
      (float)prop.maxGridSize[0] * prop.maxThreadsPerBlock) {
    printf("n is too large, please choose a smaller number!\n");
  }

  if (blocks > prop.maxGridSize[0]) {
    printf(
        "Grid size <%d> exceeds the device capability <%d>, set block size as "
        "%d (original %d)\n",
        blocks, prop.maxGridSize[0], threads * 2, threads);

    blocks /= 2;
    threads *= 2;
  }

  if (whichKernel >= 6) {
    blocks = MIN(maxBlocks, blocks);
  }
}

////////////////////////////////////////////////////////////////////////////////
// This function performs a reduction of the input data multiple times and
// measures the average reduction time.
////////////////////////////////////////////////////////////////////////////////
template <class T>
T benchmarkReduce(int n, int numThreads, int numBlocks, int maxThreads,
                  int maxBlocks, int whichKernel, int testIterations,
                  bool cpuFinalReduction, int cpuFinalThreshold,
                  StopWatchInterface *timer, T *h_odata, T *d_idata,
                  T *d_odata) {
  T gpu_result = 0;
  bool needReadBack = true;

  T *d_intermediateSums;
  checkCudaErrors(
      cudaMalloc((void **)&d_intermediateSums, sizeof(T) * numBlocks));

  // for (int i = 0; i < testIterations; ++i) {
  gpu_result = 0;

  cudaDeviceSynchronize();
  sdkStartTimer(&timer);

  // execute the kernel
  reduce<T>(n, numThreads, numBlocks, whichKernel, d_idata, d_odata);

  // check if kernel execution generated an error
  getLastCudaError("Kernel execution failed");

  if (cpuFinalReduction) {
    // sum partial sums from each block on CPU
    // copy result from device to host
    checkCudaErrors(cudaMemcpy(h_odata, d_odata, numBlocks * sizeof(T),
                                cudaMemcpyDeviceToHost));

    for (int i = 0; i < numBlocks; i++) {
      gpu_result += h_odata[i];
    }

    needReadBack = false;
  } else {
    // sum partial block sums on GPU
    int s = numBlocks;
    int kernel = whichKernel;

    while (s > cpuFinalThreshold) {
      int threads = 0, blocks = 0;
      getNumBlocksAndThreads(kernel, s, maxBlocks, maxThreads, blocks,
                              threads);
      checkCudaErrors(cudaMemcpy(d_intermediateSums, d_odata, s * sizeof(T),
                                  cudaMemcpyDeviceToDevice));
      // reduce<T>(s, threads, blocks, kernel, d_intermediateSums, d_odata);

      if (kernel < 3) {
        s = (s + threads - 1) / threads;
      } else {
        s = (s + (threads * 2 - 1)) / (threads * 2);
      }
    }

    if (s > 1) {
      // copy result from device to host
      checkCudaErrors(cudaMemcpy(h_odata, d_odata, s * sizeof(T),
                                  cudaMemcpyDeviceToHost));

      for (int i = 0; i < s; i++) {
        gpu_result += h_odata[i];
      }

      needReadBack = false;
    }
  }

  cudaDeviceSynchronize();
  sdkStopTimer(&timer);
  // }

  if (needReadBack) {
    // copy final sum from device to host
    checkCudaErrors(
        cudaMemcpy(&gpu_result, d_odata, sizeof(T), cudaMemcpyDeviceToHost));
  }
  checkCudaErrors(cudaFree(d_intermediateSums));
  return gpu_result;
}

////////////////////////////////////////////////////////////////////////////////
// The main function which runs the reduction test.
////////////////////////////////////////////////////////////////////////////////
template <class T>
bool runTest(int argc, char **argv, ReduceType datatype) {
  int size = 1 << 24;    // number of elements to reduce
  int maxThreads = 256;  // number of threads per block
  int whichKernel = 7;
  int maxBlocks = 64;
  bool cpuFinalReduction = false;
  int cpuFinalThreshold = 4;

  if (checkCmdLineFlag(argc, (const char **)argv, "n")) {
    size = getCmdLineArgumentInt(argc, (const char **)argv, "n");
  }

  if (checkCmdLineFlag(argc, (const char **)argv, "threads")) {
    maxThreads = getCmdLineArgumentInt(argc, (const char **)argv, "threads");
  }

  if (checkCmdLineFlag(argc, (const char **)argv, "kernel")) {
    whichKernel = getCmdLineArgumentInt(argc, (const char **)argv, "kernel");
  }

  if (checkCmdLineFlag(argc, (const char **)argv, "maxblocks")) {
    maxBlocks = getCmdLineArgumentInt(argc, (const char **)argv, "maxblocks");
  }

  printf("%d elements\n", size);
  printf("%d threads (max)\n", maxThreads);

  cpuFinalReduction = checkCmdLineFlag(argc, (const char **)argv, "cpufinal");

  if (checkCmdLineFlag(argc, (const char **)argv, "cputhresh")) {
    cpuFinalThreshold =
        getCmdLineArgumentInt(argc, (const char **)argv, "cputhresh");
  }


 
  // create random input data on CPU
  unsigned int bytes = size * sizeof(T);

  T *h_idata = (T *)malloc(bytes);

  for (int i = 0; i < size; i++) {
    // Keep the numbers small so we don't get truncation error in the sum
    if (datatype == REDUCE_INT) {
      h_idata[i] = (T)(rand() & 0xFF);
    } else {
      h_idata[i] = (rand() & 0xFF) / (T)RAND_MAX;
    }
  }

  int numBlocks = 0;
  int numThreads = 0;
  getNumBlocksAndThreads(whichKernel, size, maxBlocks, maxThreads, numBlocks,
                          numThreads);

  if (numBlocks == 1) {
    cpuFinalThreshold = 1;
  }

  // allocate mem for the result on host side
  T *h_odata = (T *)malloc(numBlocks * sizeof(T));

  printf("%d blocks\n\n", numBlocks);

  // allocate device memory and data
  T *d_idata = NULL;
  T *d_odata = NULL;

  checkCudaErrors(cudaMalloc((void **)&d_idata, bytes));
  checkCudaErrors(cudaMalloc((void **)&d_odata, numBlocks * sizeof(T)));

  // copy data directly to device memory
  checkCudaErrors(
      cudaMemcpy(d_idata, h_idata, bytes, cudaMemcpyHostToDevice));
  checkCudaErrors(cudaMemcpy(d_odata, h_idata, numBlocks * sizeof(T),
                              cudaMemcpyHostToDevice));

  // warm-up
  // reduce<T>(size, numThreads, numBlocks, whichKernel, d_idata, d_odata);

  int testIterations = 1;

  StopWatchInterface *timer = 0;
  sdkCreateTimer(&timer);

  T gpu_result = 0;

  gpu_result =
      benchmarkReduce<T>(size, numThreads, numBlocks, maxThreads, maxBlocks,
                          whichKernel, testIterations, cpuFinalReduction,
                          cpuFinalThreshold, timer, h_odata, d_idata, d_odata);

  double reduceTime = sdkGetAverageTimerValue(&timer) * 1e-3;
  // printf(
  //     "Reduction, Throughput = %.4f GB/s, Time = %.5f s, Size = %u Elements, "
  //     "NumDevsUsed = %d, Workgroup = %u\n",
  //     1.0e-9 * ((double)bytes) / reduceTime, reduceTime, size, 1, numThreads);

  // compute reference solution
  int precision = 0;
  double threshold = 0;
  double diff = 0;


  printf("\nGPU result = %d\n", (int)gpu_result);

  // cleanup
  sdkDeleteTimer(&timer);
  free(h_idata);
  free(h_odata);

  checkCudaErrors(cudaFree(d_idata));
  checkCudaErrors(cudaFree(d_odata));

  return true;
}
