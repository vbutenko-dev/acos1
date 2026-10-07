#ifndef EVENT_H
#define EVENT_H

// События модели и очередь событий.

#include <stdbool.h>

// Виды событий
typedef enum EventType {
    EV_THINK_END,       // философ закончил думать и проголодался
    EV_FORK_REQUEST,    // философ просит вилку
    EV_EAT_END,         // философ закончил есть
    EV_WAIT_CHECK,      // проверка, не ждёт ли философ слишком долго
    EV_TIME_LIMIT       // закончилось время моделирования
} EventType;

typedef struct Event {
    long long time;     // когда произойдёт событие (модельное время)
    long seq;           // номер добавления, чтобы события с одинаковым временем шли по порядку
    EventType type;
    int phil;           // номер философа (-1 если не нужен)
    int fork;           // номер вилки (-1 если не нужен)
} Event;

// Очередь событий - обычный массив, который увеличивается при необходимости
typedef struct EventQueue {
    Event* items;
    int size;
    int capacity;
    long nextSeq;
} EventQueue;

void EventQueueInit(EventQueue* q);
void EventQueueFree(EventQueue* q);

// Добавить событие. Возвращает false, если не хватило памяти.
bool EventQueuePush(EventQueue* q, long long time, EventType type, int phil, int fork);

// Достать самое раннее событие. Возвращает false, если очередь пуста.
bool EventQueuePop(EventQueue* q, Event* out);

#endif
