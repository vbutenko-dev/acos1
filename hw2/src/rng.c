#include "rng.h"

void RngSeed(Rng *rng, uint64_t seed) {
    rng->state = seed ^ UINT64_C(0x9E3779B97F4A7C15);
    if (rng->state == 0) {
        rng->state = 1;
    }
}

// xorshift64*: тот же генератор, который использовался в первой работе.
uint64_t RngNext(Rng *rng) {
    uint64_t state = rng->state;
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    rng->state = state;
    return state * UINT64_C(0x2545F4914F6CDD1D);
}

long long RngRange(Rng *rng, long long low, long long high) {
    uint64_t span = (uint64_t)(high - low) + 1;
    return low + (long long)(RngNext(rng) % span);
}
