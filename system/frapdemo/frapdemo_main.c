#include <nuttx/config.h>
#include <nuttx/frap.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <assert.h>

static struct frap_res g_r1;

static cpu_set_t cpu0, cpu1;

static void pin_to_cpu(int cpu)
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  (void) sched_setaffinity(0, sizeof(set), &set);
}

static void busy_us(unsigned usec)
{
  /* 简易忙等，避免在非抢占区内调用阻塞API */
  volatile unsigned long long s = 0;
  unsigned long long loops = (unsigned long long)usec * 200ULL;
  while (loops--) s += loops;
}

/* 资源持有者：先拿到 g_r1 并忙等一段时间，保证其他任务只能自旋 */
static void *owner_thread(void *arg)
{
  pin_to_cpu(1); /* CPU1 */
  printf("[owner] start (prio=%d)\n", (int)sched_get_priority_max(SCHED_FIFO)-10);
  int rc = frap_lock(&g_r1, /*spin_prio*/ 120);
  assert(rc == 0);
  printf("[owner] in CS\n");
  busy_us(200000); /* 约200ms */
  frap_unlock(&g_r1);
  printf("[owner] unlock\n");
  return NULL;
}

/* 低任务：在 CPU0 上自旋等待，并被高任务抢占后应被取消、恢复后尾插 */
static void *low_thread(void *arg)
{
  pin_to_cpu(0); /* CPU0 */
  printf("[low] request r1 (base=50, spin=60)\n");
  int rc = frap_lock(&g_r1, /*spin_prio*/ 60);
  assert(rc == 0);
  printf("[low] ENTER CS (should be after high)\n");
  busy_us(100000);
  frap_unlock(&g_r1);
  printf("[low] unlock\n");
  return NULL;
}

/* 高任务：比 low 的自旋优先级还高，触发抢占与取消 */
static void *high_thread(void *arg)
{
  pin_to_cpu(0); /* 同核，才能验证“自旋时被抢占” */
  usleep(50000); /* 等 low 已经开始自旋 */
  printf("[high] request r1 (base=80, spin=90)\n");
  int rc = frap_lock(&g_r1, /*spin_prio*/ 90);
  assert(rc == 0);
  printf("[high] ENTER CS (should be before low)\n");
  busy_us(80000);
  frap_unlock(&g_r1);
  printf("[high] unlock\n");
  return NULL;
}

// static void set_fifo_prio(int prio)
// {
//   struct sched_param sp = { .sched_priority = prio };
//   pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
// }

int main(int argc, char *argv[])
{
  /* 初始化资源（全局资源） */
  frap_res_init(&g_r1, 1, true);

  pthread_t towner, tlow, thigh;
  pthread_attr_t a_owner, a_low, a_high;

  pthread_attr_init(&a_owner);
  pthread_attr_init(&a_low);
  pthread_attr_init(&a_high);

  /* 设定线程优先级：owner=70，low=50，high=80 */
  struct sched_param sp;

  sp.sched_priority = 70; 
  pthread_attr_setschedpolicy(&a_owner, SCHED_FIFO);
  pthread_attr_setschedparam(&a_owner, &sp);
  pthread_attr_setinheritsched(&a_owner, PTHREAD_EXPLICIT_SCHED);

  sp.sched_priority = 50; 
  pthread_attr_setschedpolicy(&a_low, SCHED_FIFO);
  pthread_attr_setschedparam(&a_low, &sp);
  pthread_attr_setinheritsched(&a_low, PTHREAD_EXPLICIT_SCHED);

  sp.sched_priority = 80; 
  pthread_attr_setschedpolicy(&a_high, SCHED_FIFO);
  pthread_attr_setschedparam(&a_high, &sp);
  pthread_attr_setinheritsched(&a_high, PTHREAD_EXPLICIT_SCHED);

  /* 创建顺序：先 owner（占住资源），再 low（开始自旋），最后 high（触发抢占） */
  pthread_create(&towner, &a_owner, owner_thread, NULL);
  usleep(10000);
  pthread_create(&tlow,   &a_low,   low_thread,   NULL);
  usleep(10000);
  pthread_create(&thigh,  &a_high,  high_thread,  NULL);

  pthread_join(towner, NULL);
  pthread_join(thigh,  NULL);
  pthread_join(tlow,   NULL);

  printf("[demo] done\n");
  return 0;
}