/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file log_tail.c
 *  @brief The last lines of the log in RAM a restart keeps. BL616 only.
 *
 *  The BL616's console leaves the board through the FPGA (its UART TX goes to an FPGA pin
 *  the core passes on). While a JTAG load has the FPGA erased, nothing of the log gets out.
 *  Every line therefore also goes into a ring in the HBN RAM, with the tick in front. The
 *  partner firmware runs first after every restart and reuses the normal RAM, the end of the
 *  HBN RAM it leaves alone. A
 *  part that may hang arms the tail (log_tail_arm()), and when it ends in a restart, the
 *  next start prints the ring once the FPGA runs a core again (log_tail_dump()). Until
 *  then that start leaves the ring as it found it, its own lines only go to the console.
 *  A part that ends normally prints the lines it logged when it is disarmed, the console
 *  missed them. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "bflb_irq.h"

#include "../mcu_hw.h"
#include "log_tail.h"

#define TAIL_MAGIC 0x6c546c67u /* "glTl" */
#define TAIL_ARMED 0x61726d64u /* "armd" */
#define TAIL_SIZE  1920        /* about the last 25 lines */
#define LINE_MAX   256         /* longer lines are cut */

typedef struct {
  uint32_t magic;     /* TAIL_MAGIC once the ring is set up, power on leaves random words */
  uint32_t armed;     /* TAIL_ARMED while an armed part runs */
  uint32_t head;      /* bytes written so far, the ring position is head % TAIL_SIZE */
  uint32_t trap[4];   /* mcause, mepc, mtval and ra of a CPU exception, mcause 0 for none */
  char trap_task[16]; /* the task the exception hit */
  char buf[TAIL_SIZE];
} tail_t;

/* the upper half of the 4 KB HBN RAM at 0x20010000, the SDK's hbn_ram section takes the
   start (bl616_common.ld.in), 2 KB from 0x20010800 hold the tail */
#define tail (*(tail_t *)0x20010800u)
_Static_assert(sizeof(tail_t) <= 2048, "the tail must fit 2 KB of HBN RAM");

static bool checked; /* the first line of this run has looked at the ring */
static uint32_t armed_at; /* head when the part was armed */
static bool frozen;  /* the ring holds an armed run's tail, kept until log_tail_dump() */

void log_tail_printf(const char *fmt, ...) {
  char line[LINE_MAX];
  va_list ap;
  int n = snprintf(line, sizeof(line), "[%lu] ", (unsigned long)xTaskGetTickCount());
  int start = n;
  va_start(ap, fmt);
  n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
  va_end(ap);
  if(n >= (int)sizeof(line)) n = sizeof(line) - 1;
  printf("%s", line + start); /* the console as before, without the tick */

  // the ring takes the line with interrupts off, a line from an interrupt as well
  uintptr_t flags = bflb_irq_save();
  if(!checked) {
    checked = true;
    if(tail.magic != TAIL_MAGIC) {
      tail.magic   = TAIL_MAGIC;
      tail.armed   = 0;
      tail.head    = 0;
      tail.trap[0] = 0;
    } else if(tail.armed == TAIL_ARMED || tail.trap[0]) frozen = true;
  }
  if(!frozen)
    for(int i = 0; i < n; i++) tail.buf[tail.head++ % TAIL_SIZE] = line[i];
  bflb_irq_restore(flags);
}

/* the ring from byte from to head, as far as it still holds them */
static void print_ring(uint32_t from) {
  uint32_t head = tail.head;
  if(head - from > TAIL_SIZE) from = head - TAIL_SIZE;
  for(uint32_t i = from; i != head; i++) putchar(tail.buf[i % TAIL_SIZE]);
}

void log_tail_print_armed(void) {
  printf("\r\n==== log of the armed part so far ====\r\n");
  print_ring(armed_at);
  printf("==== end of the armed part so far ====\r\n");
}

void log_tail_arm(bool on) {
  if(frozen) return;
  if(on) {
    armed_at   = tail.head;
    tail.armed = TAIL_ARMED;
    return;
  }
  tail.armed = 0;
  printf("\r\n==== log of the armed part, the console may have missed it ====\r\n");
  print_ring(armed_at);
  printf("==== end of the armed part ====\r\n");
}

void log_tail_dump(void) {
  // only a start that found an armed run's tail, power on and the restarts of a core switch go on
  if(!frozen) return;
  uint32_t head = tail.head, n = head < TAIL_SIZE ? head : TAIL_SIZE;
  printf("\r\n==== log tail of the run before the restart (%s), %lu bytes ====\r\n",
         mcu_hw_restarted() ? "watchdog" : "other reset", (unsigned long)n);
  if(tail.trap[0])
    printf("CPU exception in task %.16s: mcause %08lx mepc %08lx mtval %08lx ra %08lx\r\n", tail.trap_task,
           (unsigned long)tail.trap[0], (unsigned long)tail.trap[1], (unsigned long)tail.trap[2],
           (unsigned long)tail.trap[3]);
  print_ring(head - n);
  printf("\r\n==== end of the log tail ====\r\n");
  tail.armed   = 0;
  tail.trap[0] = 0;
  frozen       = false;
}

/* the SDK's exception_entry (vector.S calls it, the link wraps it): the exception goes
   into the tail first, the SDK prints it on the console, which may not get out, and restarts */
void __real_exception_entry(uintptr_t *regs);
void __wrap_exception_entry(uintptr_t *regs) {
  uintptr_t cause, epc, tval;
  __asm__ volatile("csrr %0, mcause" : "=r"(cause));
  __asm__ volatile("csrr %0, mepc" : "=r"(epc));
  __asm__ volatile("csrr %0, mtval" : "=r"(tval));
  // ecalls come back, only real faults are kept (the SDK returns for causes 8 and 11)
  if((cause & 0x3ff) != 8 && (cause & 0x3ff) != 11) {
    tail.magic   = TAIL_MAGIC;
    tail.trap[0] = cause ? cause : 0x80000000u; /* never 0, that means none */
    tail.trap[1] = epc;
    tail.trap[2] = tval;
    tail.trap[3] = regs ? regs[1] : 0; /* x1, ra, in the frame of vector.S */
    const char *name = pcTaskGetName(NULL);
    for(int i = 0; i < 16; i++) tail.trap_task[i] = name ? name[i] : 0;
  }
  __real_exception_entry(regs);
}
