#include "rng.h"

// Начальное состояние
void RngSeed(Rng* rng, uint64_t seed) {
    rng->state = seed ^ 0x9E3779B97F4A7C15ULL;
    if (rng->state == 0) {
        rng->state = 1;
    }
}

// Шаг xorshift64*
uint64_t RngNext(Rng* rng) {
    uint64_t x = rng->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng->state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

// Случайное число от lo до hi включительно
long long RngRange(Rng* rng, long long lo, long long hi) {
    uint64_t span = (uint64_t)(hi - lo) + 1;
    return lo + (long long)(RngNext(rng) % span);
}
