#!/bin/sh

# panic: Bad tailq NEXT(0xfffffe0079608f00->tqh_last) != NULL
# cpuid = 20
# time = 1616404997
# KDB: stack backtrace:
# db_trace_self_wrapper() at db_trace_self_wrapper+0x2b/frame 0xfffffe01af6d8580
# vpanic() at vpanic+0x181/frame 0xfffffe01af6d85d0
# panic() at panic+0x43/frame 0xfffffe01af6d8630
# sctp_ss_default_add() at sctp_ss_default_add+0xd7/frame 0xfffffe01af6d8660
# sctp_lower_sosend() at sctp_lower_sosend+0x3b24/frame 0xfffffe01af6d8830
# sctp_sosend() at sctp_sosend+0x344/frame 0xfffffe01af6d8950
# sosend() at sosend+0x66/frame 0xfffffe01af6d8980
# kern_sendit() at kern_sendit+0x1ec/frame 0xfffffe01af6d8a10
# sendit() at sendit+0x1db/frame 0xfffffe01af6d8a60
# sys_sendmsg() at sys_sendmsg+0x61/frame 0xfffffe01af6d8ac0
# amd64_syscall() at amd64_syscall+0x147/frame 0xfffffe01af6d8bf0
# fast_syscall_common() at fast_syscall_common+0xf8/frame 0xfffffe01af6d8bf0
# --- syscall (0, FreeBSD ELF64, nosys), rip = 0x8003b042a, rsp = 0x7fffdffdcf68, rbp = 0x7fffdffdcf90 ---
# KDB: enter: panic
# [ thread pid 7402 tid 730532 ]
# Stopped at      kdb_enter+0x37: movq    $0,0x1286f8e(%rip)
# db> x/s version
# version: FreeBSD 14.0-CURRENT #0 main-n245565-25bfa448602: Mon Mar 22 09:13:03 CET 2021
# pho@t2.osted.lan:/usr/src/sys/amd64/compile/PHO\012
# db>

[ `uname -p` != "amd64" ] && exit 0

. ../default.cfg
kldstat -v | grep -q sctp || kldload sctp.ko

cat > /tmp/syzkaller31.c <<EOF
// https://syzkaller.appspot.com/bug?id=57c78914e36b822eaa173cfd681e4f63822e0844
// autogenerated by syzkaller (https://github.com/google/syzkaller)
// Reported-by: syzbot+ed6e8de942351d0309f4@syzkaller.appspotmail.com

#define _GNU_SOURCE

#include <sys/types.h>

#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/endian.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void kill_and_wait(int pid, int* status)
{
  kill(pid, SIGKILL);
  while (waitpid(-1, status, 0) != pid) {
  }
}

static void sleep_ms(uint64_t ms)
{
  usleep(ms * 1000);
}

static uint64_t current_time_ms(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts))
    exit(1);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void thread_start(void* (*fn)(void*), void* arg)
{
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 128 << 10);
  int i = 0;
  for (; i < 100; i++) {
    if (pthread_create(&th, &attr, fn, arg) == 0) {
      pthread_attr_destroy(&attr);
      return;
    }
    if (errno == EAGAIN) {
      usleep(50);
      continue;
    }
    break;
  }
  exit(1);
}

typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int state;
} event_t;

static void event_init(event_t* ev)
{
  if (pthread_mutex_init(&ev->mu, 0))
    exit(1);
  if (pthread_cond_init(&ev->cv, 0))
    exit(1);
  ev->state = 0;
}

static void event_reset(event_t* ev)
{
  ev->state = 0;
}

static void event_set(event_t* ev)
{
  pthread_mutex_lock(&ev->mu);
  if (ev->state)
    exit(1);
  ev->state = 1;
  pthread_mutex_unlock(&ev->mu);
  pthread_cond_broadcast(&ev->cv);
}

static void event_wait(event_t* ev)
{
  pthread_mutex_lock(&ev->mu);
  while (!ev->state)
    pthread_cond_wait(&ev->cv, &ev->mu);
  pthread_mutex_unlock(&ev->mu);
}

static int event_isset(event_t* ev)
{
  pthread_mutex_lock(&ev->mu);
  int res = ev->state;
  pthread_mutex_unlock(&ev->mu);
  return res;
}

static int event_timedwait(event_t* ev, uint64_t timeout)
{
  uint64_t start = current_time_ms();
  uint64_t now = start;
  pthread_mutex_lock(&ev->mu);
  for (;;) {
    if (ev->state)
      break;
    uint64_t remain = timeout - (now - start);
    struct timespec ts;
    ts.tv_sec = remain / 1000;
    ts.tv_nsec = (remain % 1000) * 1000 * 1000;
    pthread_cond_timedwait(&ev->cv, &ev->mu, &ts);
    now = current_time_ms();
    if (now - start > timeout)
      break;
  }
  int res = ev->state;
  pthread_mutex_unlock(&ev->mu);
  return res;
}

struct thread_t {
  int created, call;
  event_t ready, done;
};

static struct thread_t threads[16];
static void execute_call(int call);
static int running;

static void* thr(void* arg)
{
  struct thread_t* th = (struct thread_t*)arg;
  for (;;) {
    event_wait(&th->ready);
    event_reset(&th->ready);
    execute_call(th->call);
    __atomic_fetch_sub(&running, 1, __ATOMIC_RELAXED);
    event_set(&th->done);
  }
  return 0;
}

static void execute_one(void)
{
  int i, call, thread;
  int collide = 0;
again:
  for (call = 0; call < 3; call++) {
    for (thread = 0; thread < (int)(sizeof(threads) / sizeof(threads[0]));
         thread++) {
      struct thread_t* th = &threads[thread];
      if (!th->created) {
        th->created = 1;
        event_init(&th->ready);
        event_init(&th->done);
        event_set(&th->done);
        thread_start(thr, th);
      }
      if (!event_isset(&th->done))
        continue;
      event_reset(&th->done);
      th->call = call;
      __atomic_fetch_add(&running, 1, __ATOMIC_RELAXED);
      event_set(&th->ready);
      if (collide && (call % 2) == 0)
        break;
      event_timedwait(&th->done, 50);
      break;
    }
  }
  for (i = 0; i < 100 && __atomic_load_n(&running, __ATOMIC_RELAXED); i++)
    sleep_ms(1);
  if (!collide) {
    collide = 1;
    goto again;
  }
}

static void execute_one(void);

#define WAIT_FLAGS 0

static void loop(void)
{
  int iter __unused = 0;
  for (;; iter++) {
    int pid = fork();
    if (pid < 0)
      exit(1);
    if (pid == 0) {
      execute_one();
      exit(0);
    }
    int status = 0;
    uint64_t start = current_time_ms();
    for (;;) {
      if (waitpid(-1, &status, WNOHANG | WAIT_FLAGS) == pid)
        break;
      sleep_ms(1);
      if (current_time_ms() - start < 5000) {
        continue;
      }
      kill_and_wait(pid, &status);
      break;
    }
  }
}

uint64_t r[2] = {0xffffffffffffffff, 0xffffffffffffffff};

void execute_call(int call)
{
  intptr_t res = 0;
  switch (call) {
  case 0:
    res = syscall(SYS_socket, 2ul, 5ul, 0x84);
    if (res != -1)
      r[0] = res;
    break;
  case 1:
    res = syscall(SYS_fcntl, r[0], 0ul, r[0]);
    if (res != -1)
      r[1] = res;
    break;
  case 2:
    *(uint64_t*)0x20000040 = 0x20000000;
    *(uint8_t*)0x20000000 = 0x1c;
    *(uint8_t*)0x20000001 = 0x1c;
    *(uint16_t*)0x20000002 = htobe16(0x4e22);
    *(uint32_t*)0x20000004 = 0;
    *(uint8_t*)0x20000008 = 0;
    *(uint8_t*)0x20000009 = 0;
    *(uint8_t*)0x2000000a = 0;
    *(uint8_t*)0x2000000b = 0;
    *(uint8_t*)0x2000000c = 0;
    *(uint8_t*)0x2000000d = 0;
    *(uint8_t*)0x2000000e = 0;
    *(uint8_t*)0x2000000f = 0;
    *(uint8_t*)0x20000010 = 0;
    *(uint8_t*)0x20000011 = 0;
    *(uint8_t*)0x20000012 = -1;
    *(uint8_t*)0x20000013 = -1;
    *(uint32_t*)0x20000014 = htobe32(0x203);
    *(uint32_t*)0x20000018 = 0;
    *(uint32_t*)0x20000048 = 0x1c;
    *(uint64_t*)0x20000050 = 0x20000100;
    *(uint64_t*)0x20000100 = 0x20000080;
    memcpy((void*)0x20000080, "\000", 1);
    *(uint64_t*)0x20000108 = 1;
    *(uint32_t*)0x20000058 = 1;
    *(uint64_t*)0x20000060 = 0x20000200;
    memcpy(
        (void*)0x20000200,
        "\x1c\x00\x00\x00\x84\x00\x00\x00\x01\x00\x00\x00\x40\x08\xec\x70\xa0",
        17);
    *(uint32_t*)0x20000068 = 0x1c;
    *(uint32_t*)0x2000006c = 0;
    syscall(SYS_sendmsg, r[1], 0x20000040ul, 0ul);
    break;
  }
}
int main(void)
{
  syscall(SYS_mmap, 0x20000000ul, 0x1000000ul, 7ul, 0x1012ul, -1, 0ul);
  loop();
  return 0;
}
EOF
mycc -o /tmp/syzkaller31 -Wall -Wextra -O0 /tmp/syzkaller31.c -lpthread ||
    exit 1

(cd ../testcases/swap; ./swap -t 1m -i 20 -h > /dev/null 2>&1) &
(cd /tmp; timeout 3m ./syzkaller31)
while pkill swap; do :; done
wait

rm -rf /tmp/syzkaller31 /tmp/syzkaller31.c /tmp/syzkaller.*
exit 0
