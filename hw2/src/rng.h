#ifndef HW2_RNG_H
#define HW2_RNG_H

#include <stdint.h>

// У каждого философа свой генератор случайных чисел.
typedef struct Rng {
    uint64_t state;
} Rng;

// Начальное состояние генератора по заданному seed.
void RngSeed(Rng *rng, uint64_t seed);
uint64_t RngNext(Rng *rng);

// Случайное целое от low до high включительно.
long long RngRange(Rng *rng, long long low, long long high);

#endif
