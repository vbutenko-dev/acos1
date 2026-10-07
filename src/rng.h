#ifndef RNG_H
#define RNG_H


// Собственный генератор псевдослучайных чисел.

#include <stdint.h>

// Состояние генератора.
typedef struct Rng {
    uint64_t state;     // внутреннее состояние 
} Rng;

// Начальная установка генератора по значению seed
void RngSeed(Rng* rng, uint64_t seed);

// Очередное 64-битное псевдослучайное число
uint64_t RngNext(Rng* rng);

// Случайное целое в диапазоне [lo, hi] (lo <= hi)
long long RngRange(Rng* rng, long long lo, long long hi);

#endif // RNG_H
