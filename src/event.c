#include <stdlib.h>
#include "event.h"

void EventQueueInit(EventQueue* q) {
    q->items = NULL;
    q->size = 0;
    q->capacity = 0;
    q->nextSeq = 0;
}

void EventQueueFree(EventQueue* q) {
    free(q->items);
    EventQueueInit(q);
}

bool EventQueuePush(EventQueue* q, long long time, EventType type, int phil, int fork) {
    // если место кончилось, увеличиваем массив в 2 раза
    if (q->size == q->capacity) {
        int newCapacity = (q->capacity == 0) ? 16 : q->capacity * 2;
        Event* newItems = realloc(q->items, newCapacity * sizeof(Event));
        if (newItems == NULL) {
            return false;
        }
        q->items = newItems;
        q->capacity = newCapacity;
    }
    Event* e = &q->items[q->size];
    e->time = time;
    e->seq = q->nextSeq;
    e->type = type;
    e->phil = phil;
    e->fork = fork;
    q->size++;
    q->nextSeq++;
    return true;
}

bool EventQueuePop(EventQueue* q, Event* out) {
    if (q->size == 0) {
        return false;
    }
    // ищем самое раннее событие (при равном времени добавленное раньше)
    int best = 0;
    for (int i = 1; i < q->size; i++) {
        Event* a = &q->items[i];
        Event* b = &q->items[best];
        if (a->time < b->time || (a->time == b->time && a->seq < b->seq)) {
            best = i;
        }
    }
    *out = q->items[best];
    // на его место ставим последний элемент
    q->items[best] = q->items[q->size - 1];
    q->size--;
    return true;
}
