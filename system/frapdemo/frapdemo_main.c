/****************************************************************************
 * apps/system/frapdemo/frapdemo_main.c
 *
 * Revised FRAP demo to work with single-arg frap_lock(res):
 *  - frap_set_spin_prio is applied by main thread after creating workers
 *  - workers wait on a condition until the table is applied
 *  - workers call frap_lock(&res) (kernel reads their spin-prio)
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <nuttx/frap.h>

#include "frap_table_generated.h"

/* Number of worker threads (must match JSON pid_hint indices) */
#define WORKER_NUM 8

static pthread_t workers[WORKER_NUM];

/* FRAP resources */
static struct frap_res g_r0;
static struct frap_res g_r1;
static struct frap_res g_r2;
static struct frap_res g_r3;

/* Start barrier: ensure main sets spin-prio before workers proceed */
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
/* start_cond 是一个 条件变量，它本身不是“真假值”，而是用于线程之间通知的一种同步原语 */
static pthread_cond_t  start_cond = PTHREAD_COND_INITIALIZER;
static int             start_flag = 0;

/* Busy-wait helper */
static void busy_work(unsigned loops)
{
  volatile unsigned long long s = 0;
  while (loops--) s += loops;
}

/* Pin to CPU helper (best-effort) */
static void pin_to_cpu(int cpu)
{
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPUAFFINITY)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  (void)sched_setaffinity(0, sizeof(set), &set);
#else
  (void)cpu;
#endif
}

/* Worker: wait for start flag */
/* 主线程会在创建线程、下发表后把 start_flag 置为 1 并唤醒所有 worker，这保证 worker 第一次调用 frap_lock 时表已经写入内核。 */
static void wait_for_start(void)
{
  pthread_mutex_lock(&start_lock);
  while (!start_flag)
    {
      /* 
        pthread_cond_wait：
        调用线程会 原子地 解锁 start_lock（互斥锁），然后睡眠等待 start_cond 被其他线程 broadcast 唤醒
        当被唤醒后，pthread_cond_wait 会重新加锁 start_lock 并返回 
      */
      pthread_cond_wait(&start_cond, &start_lock);
    }
  pthread_mutex_unlock(&start_lock);
}

/* Worker threads — they call frap_lock(&res) (kernel reads spin-prio) */

/* hot0 (index 0): CPU0 */
static void *worker_hot0(void *arg)
{
  (void)arg;
  pin_to_cpu(0);
  wait_for_start();

  for (int i = 0; i < 200; i++)
    {
      frap_lock(&g_r0);
      busy_work(2000);
      frap_unlock(&g_r0);

      frap_lock(&g_r1);
      busy_work(4000);
      frap_unlock(&g_r1);

      usleep(1000);
    }
  return NULL;
}

/* hot1 (index 1): CPU0 */
static void *worker_hot1(void *arg)
{
  (void)arg;
  pin_to_cpu(0);
  wait_for_start();

  for (int i = 0; i < 200; i++)
    {
      frap_lock(&g_r0);
      busy_work(1800);
      frap_unlock(&g_r0);

      frap_lock(&g_r1);
      busy_work(3500);
      frap_unlock(&g_r1);

      usleep(1200);
    }
  return NULL;
}

/* mid0 (index 2): CPU0 */
static void *worker_mid0(void *arg)
{
  (void)arg;
  pin_to_cpu(0);
  wait_for_start();

  for (int i = 0; i < 200; i++)
    {
      frap_lock(&g_r0);
      busy_work(3000);
      frap_unlock(&g_r0);

      frap_lock(&g_r2);
      busy_work(2500);
      frap_unlock(&g_r2);

      usleep(1500);
    }
  return NULL;
}

/* mid1 (index 3): CPU0 */
static void *worker_mid1(void *arg)
{
  (void)arg;
  pin_to_cpu(0);
  wait_for_start();

  for (int i = 0; i < 200; i++)
    {
      frap_lock(&g_r2);
      busy_work(2400);
      frap_unlock(&g_r2);

      frap_lock(&g_r3);
      busy_work(8000); /* long CS */
      frap_unlock(&g_r3);

      usleep(2000);
    }
  return NULL;
}

/* remoteA0 (index 4): CPU1 */
static void *worker_remoteA0(void *arg)
{
  (void)arg;
  pin_to_cpu(1);
  wait_for_start();

  for (int i = 0; i < 200; i++)
    {
      frap_lock(&g_r1);
      busy_work(3000);
      frap_unlock(&g_r1);
      usleep(4000);
    }
  return NULL;
}

/* remoteA1 (index 5): CPU1 */
static void *worker_remoteA1(void *arg)
{
  (void)arg;
  pin_to_cpu(1);
  wait_for_start();

  for (int i = 0; i < 200; i++)
    {
      frap_lock(&g_r1);
      busy_work(3200);
      frap_unlock(&g_r1);

      frap_lock(&g_r3);
      busy_work(6000);
      frap_unlock(&g_r3);

      usleep(5000);
    }
  return NULL;
}

/* remoteB0 (index 6): CPU2 */
static void *worker_remoteB0(void *arg)
{
  (void)arg;
  pin_to_cpu(2);
  wait_for_start();

  for (int i = 0; i < 200; i++)
    {
      frap_lock(&g_r1);
      busy_work(2200);
      frap_unlock(&g_r1);

      usleep(1000);

      frap_lock(&g_r1);
      busy_work(2200);
      frap_unlock(&g_r1);

      usleep(3000);
    }
  return NULL;
}

/* background (index 7): CPU2 */
static void *worker_background(void *arg)
{
  (void)arg;
  pin_to_cpu(2);
  wait_for_start();

  for (int i = 0; i < 200; i++)
    {
      frap_lock(&g_r3);
      busy_work(2000);
      frap_unlock(&g_r3);
      usleep(7000);
    }
  return NULL;
}

/* mapping table */
static void *(*worker_table[WORKER_NUM])(void *) = {
  worker_hot0,
  worker_hot1,
  worker_mid0,
  worker_mid1,
  worker_remoteA0,
  worker_remoteA1,
  worker_remoteB0,
  worker_background
};

int main(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;
  printf("\n[FRAPDEMO] Starting FRAP demo (table-driven)...\n");

  /* init FRAP resources */
  frap_res_init(&g_r0, 0, true);
  frap_res_init(&g_r1, 1, true);
  frap_res_init(&g_r2, 2, true);
  frap_res_init(&g_r3, 3, true);

  /* Create worker threads (they will wait on start_cond) */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_attr_t attr;
      pthread_attr_init(&attr);

      struct sched_param param;
      int base_prio = 50;
      switch (i)
        {
        case 0: base_prio = 240; break;
        case 1: base_prio = 238; break;
        case 2: base_prio = 200; break;
        case 3: base_prio = 190; break;
        case 4: base_prio = 120; break;
        case 5: base_prio = 110; break;
        case 6: base_prio = 115; break;
        case 7: base_prio = 60;  break;
        default: base_prio = 50; break;
        }

      param.sched_priority = base_prio;
      pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
      pthread_attr_setschedparam(&attr, &param);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

      int ret = pthread_create(&workers[i], &attr, worker_table[i], NULL);
      if (ret != 0)
        {
          printf("Failed to create worker %d (ret=%d)\n", i, ret);
        }
      pthread_attr_destroy(&attr);
      usleep(10000);
    }

  /* ------------------ Apply FRAP spin-priority table ------------------ */
  printf("[FRAPDEMO] Applying generated FRAP spin-priority table...\n");
  for (int e = 0; e < frap_generated_table_len; e++)
    {
      const struct frap_cfg_entry *ent = &frap_generated_table[e];
      int idx = ent->pid_hint;
      if (idx < 0 || idx >= WORKER_NUM)
        {
          printf("  SKIP invalid pid_hint %d\n", idx);
          continue;
        }

      /* NOTE:
       * frap_set_spin_prio expects a pid (task id). Using pthread_t -> pid cast is common in NuttX,
       * but if your platform differs, you should call getpid() from worker or provide another mapping.
       */
      pthread_t tid = workers[idx];
      int r = frap_set_spin_prio((pid_t)tid, ent->resid, ent->spin_prio);
      if (r != OK)
        {
          printf("  FAIL pid=%lu resid=%d prio=%d (ret=%d)\n",
                 (unsigned long)tid, ent->resid, ent->spin_prio, r);
        }
      else
        {
          printf("  OK   pid=%lu resid=%d prio=%d\n",
                 (unsigned long)tid, ent->resid, ent->spin_prio);
        }
    }

  /* Release workers to start; now table is applied */
  pthread_mutex_lock(&start_lock);
  start_flag = 1;
  /*
    pthread_cond_broadcast：
    把等待在 start_cond 上的 所有线程 都唤醒，常用于“把所有 worker 放行”这种场景
  */
  pthread_cond_broadcast(&start_cond);
  pthread_mutex_unlock(&start_lock);

  /* Join workers */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_join(workers[i], NULL);
    }

  printf("[FRAPDEMO] demo finished\n");
  return 0;
}
