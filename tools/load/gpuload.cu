#include <cstdio>
#include <cstdlib>
#include <chrono>
__global__ void spin(float *p, int n, int iters) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float v = p[i];
    for (int k = 0; k < iters; k++) v = v * 1.000001f + 0.5f;
    p[i] = v;
}
int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 20;
    int duty_ms = argc > 2 ? atoi(argv[2]) : 0;   /* sleep between launches to get partial util */
    size_t n = 64u << 20;                          /* 256 MB of floats */
    float *d; cudaMalloc(&d, n * sizeof(float)); cudaMemset(d, 0, n * sizeof(float));
    auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    long launches = 0;
    while (std::chrono::steady_clock::now() < end) {
        spin<<<(int)(n / 256), 256>>>(d, (int)n, 200);
        cudaDeviceSynchronize(); launches++;
        if (duty_ms) { struct timespec ts = {0, duty_ms * 1000000L}; nanosleep(&ts, NULL); }
    }
    printf("gpuload done: %ld launches\n", launches);
    cudaFree(d); return 0;
}
