/*
 * Simple simulation of the flight-sequencer case study (one core, FreeRTOS via CMSIS-RTOS v2).
 *
 *   Sensor1Task   5 ms   stands in for the Sensor 1 RX ISR  (highest priority, like an ISR)
 *   Sensor2Task  10 ms   stands in for the Sensor 2 RX ISR
 *   SequencerTask 10 ms  reads both sensors + feedback, computes, "sends" the command
 *   SupervisorTask 10 ms checks that every task is alive, "kicks the watchdog"
 *   SlowTask     200 ms  heavy computation, result is fed back to the Sequencer
 *   LoggerTask           prints queued messages on USART3 (lowest priority)
 *
 * Work time of each task is simulated with a busy wait (the case study's A10 placeholders).
 */
#include "flight_sequencer_app.h"
#include "main.h"
#include "cmsis_os.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- timing numbers (placeholders) ---- */
#define SEQ_WORK_US   2000u
#define SUP_WORK_US    300u
#define SLOW_WORK_US 60000u
#define DEADLINE_US  10000u

/* ---- priorities: sensor tasks above everything, like interrupts ---- */
#define PRIO_SENSOR osPriorityRealtime
#define PRIO_SEQ    osPriorityHigh
#define PRIO_SUP    osPriorityAboveNormal
#define PRIO_SLOW   osPriorityBelowNormal
#define PRIO_LOG    osPriorityLow

extern UART_HandleTypeDef huart3;

/* cycle counter (can be overridden for a PC build) */
#ifndef CYC_NOW
#define CYC_NOW() (DWT->CYCCNT)
#endif

static void busy_us(uint32_t us)           /* simulate "this task needs us microseconds of CPU" */
{
    uint32_t start = CYC_NOW(), ticks = us * (SystemCoreClock / 1000000u);
    while ((CYC_NOW() - start) < ticks) { }
}
static uint32_t now_us(void) { return CYC_NOW() / (SystemCoreClock / 1000000u); }

/* ---- printing: tasks only queue text, LoggerTask sends it, so no task waits for the UART ---- */
typedef struct { char s[64]; } msg_t;
static osMessageQueueId_t log_q;

static void print(const char *fmt, ...)
{
    msg_t m;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m.s, sizeof m.s, fmt, ap);
    va_end(ap);
    osMessageQueuePut(log_q, &m, 0, 0);    /* if the queue is full the line is simply dropped */
}
static void LoggerTask(void *arg)
{
    (void)arg;
    msg_t m;
    for (;;) {
        if (osMessageQueueGet(log_q, &m, NULL, osWaitForever) == osOK)
            HAL_UART_Transmit(&huart3, (uint8_t *)m.s, (uint16_t)strlen(m.s), 100);
    }
}

/* ---- "latest value" mailbox: one writer, two slots, writer fills the idle slot then flips ---- */
typedef struct { int32_t d[2][4]; volatile uint32_t latest; volatile uint32_t count; } mbox_t;

static void mbox_put(mbox_t *m, const int32_t v[4])
{
    uint32_t w = m->latest ^ 1u;
    memcpy(m->d[w], v, 16);
    __DMB();
    m->latest = w;
    m->count++;
}
static int mbox_get(const mbox_t *m, int32_t v[4])      /* 0 = nothing published yet */
{
    if (m->count == 0) return 0;
    memcpy(v, m->d[m->latest], 16);
    return 1;
}

static mbox_t mb_s1, mb_s2;        /* sensor data  -> Sequencer */
static mbox_t mb_snap, mb_fb;      /* Sequencer -> Slow task, Slow task -> Sequencer */
static volatile uint32_t seq_alive, slow_alive;

/* ---- tasks ---- */
static void Sensor1Task(void *arg)
{
    (void)arg;
    uint32_t n = 0, next = osKernelGetTickCount();
    for (;;) {
        next += 5;
        osDelayUntil(next);
        n++;
        int32_t v[4] = { 100 + (int32_t)(n % 7), 200, 300, 400 };    /* fake measurement */
        mbox_put(&mb_s1, v);
        if (n % 200 == 0) print("[%5lu ms] Sensor1   frames=%lu\r\n", (unsigned long)osKernelGetTickCount(), (unsigned long)n);
    }
}

static void Sensor2Task(void *arg)
{
    (void)arg;
    uint32_t n = 0, next = osKernelGetTickCount() + 3;           /* own clock: not in phase with Sensor 1 */
    for (;;) {
        next += 10;
        osDelayUntil(next);
        n++;
        int32_t v[4] = { 100 - (int32_t)(n % 5), 200, 300, 400 };
        mbox_put(&mb_s2, v);
        if (n % 100 == 0) print("[%5lu ms] Sensor2   frames=%lu\r\n", (unsigned long)osKernelGetTickCount(), (unsigned long)n);
    }
}

static void SequencerTask(void *arg)
{
    (void)arg;
    uint32_t cycle = 0, next = osKernelGetTickCount(), max_us = 0, missed = 0;
    int32_t a[4] = {0}, b[4] = {0}, fb[4] = {0}, out[4] = {0};
    for (;;) {
        next += 10;
        osDelayUntil(next);
        uint32_t t0 = now_us();
        cycle++;

        mbox_get(&mb_s1, a);                 /* read inputs once */
        mbox_get(&mb_s2, b);
        mbox_get(&mb_fb, fb);                /* if there is no new feedback we keep the old one */
        for (int i = 0; i < 4; i++) out[i] = (a[i] + b[i]) / 2 + fb[0];   /* placeholder math */
        busy_us(SEQ_WORK_US);                /* simulated computation time */

        int32_t snap[4] = { (int32_t)cycle, out[0], out[1], out[2] };
        mbox_put(&mb_snap, snap);            /* for the slow task */
        seq_alive++;

        uint32_t run = now_us() - t0;
        if (run > max_us) max_us = run;
        if (run > DEADLINE_US) missed++;
        if (cycle % 100 == 0) {
            if (missed) print("[%5lu ms] Sequencer cycle=%lu DEADLINE MISSED x%lu\r\n", (unsigned long)osKernelGetTickCount(), (unsigned long)cycle, (unsigned long)missed);
            else        print("[%5lu ms] Sequencer cycle=%lu run=%luus OK\r\n", (unsigned long)osKernelGetTickCount(), (unsigned long)cycle, (unsigned long)max_us);
        }
    }
}

static void SupervisorTask(void *arg)
{
    (void)arg;
    /* task name, counter it increments, allowed silent cycles */
    struct { const char *name; volatile uint32_t *cnt; uint32_t last, silent, limit; } t[4] = {
        { "Sensor1",   &mb_s1.count, 0, 0, 3 },
        { "Sensor2",   &mb_s2.count, 0, 0, 3 },
        { "Sequencer", &seq_alive,   0, 0, 3 },
        { "SlowTask",  &slow_alive,  0, 0, 25 },
    };
    uint32_t cycle = 0, kicks = 0, next = osKernelGetTickCount();
    for (;;) {
        next += 10;
        osDelayUntil(next);
        cycle++;
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            uint32_t v = *t[i].cnt;
            if (v != t[i].last) { t[i].last = v; t[i].silent = 0; } else t[i].silent++;
            if (t[i].silent >= t[i].limit) {
                ok = 0;
                if (t[i].silent == t[i].limit)
                    print("[%5lu ms] Supervisor ALARM: %s stopped\r\n", (unsigned long)osKernelGetTickCount(), t[i].name);
            }
        }
        if (ok) kicks++;                     /* real target: kick the hardware watchdog here */
        busy_us(SUP_WORK_US);
        if (cycle % 100 == 0)
            print("[%5lu ms] Supervisor %s\r\n", (unsigned long)osKernelGetTickCount(), ok ? "all alive" : "FAULT");
    }
}

static void SlowTask(void *arg)
{
    (void)arg;
    uint32_t next = osKernelGetTickCount();
    for (;;) {
        next += 200;
        osDelayUntil(next);
        slow_alive++;
        int32_t snap[4];
        if (!mbox_get(&mb_snap, snap)) continue;
        uint32_t t0 = osKernelGetTickCount();
        busy_us(SLOW_WORK_US);               /* heavy computation */
        int32_t fb[4] = { snap[1] / 100, 0, 0, 0 };
        mbox_put(&mb_fb, fb);
        if (slow_alive % 5 == 0)
            print("[%5lu ms] Slow      took %lu ms, feedback=%ld\r\n\r\n",
                  (unsigned long)osKernelGetTickCount(), (unsigned long)(osKernelGetTickCount() - t0), (long)fb[0]);
    }
}

/* ---- start-up: call once before osKernelStart() ---- */
static void spawn(osThreadFunc_t fn, const char *name, uint32_t stack_words, osPriority_t prio)
{
    osThreadAttr_t a = { .name = name, .stack_size = stack_words * 4u, .priority = prio };
    osThreadNew(fn, NULL, &a);
}

void FlightSequencer_Start(void)
{
#ifndef CYC_NOW_OVERRIDE
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* enable the cycle counter */
    DWT->LAR = 0xC5ACCE55u;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif
    log_q = osMessageQueueNew(10, sizeof(msg_t), NULL);
    print("\r\n=== Flight Sequencer simulation started ===\r\n");

    spawn(LoggerTask,     "Logger",     512, PRIO_LOG);
    spawn(Sensor1Task,    "Sensor1",    384, PRIO_SENSOR);
    spawn(Sensor2Task,    "Sensor2",    384, PRIO_SENSOR);
    spawn(SequencerTask,  "Sequencer",  384, PRIO_SEQ);
    spawn(SupervisorTask, "Supervisor", 384, PRIO_SUP);
    spawn(SlowTask,       "Slow",       384, PRIO_SLOW);
}
