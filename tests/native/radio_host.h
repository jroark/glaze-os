/* Native platform shim for the unmodified radio worker. */
#ifndef RADIO_HOST_H
#define RADIO_HOST_H
#define GLAZE_SYS_H
#define GLAZE_THREAD_H
#define GLAZE_NET_H
#include <glaze/abi.h>
#include <pthread.h>
struct gz_mutex { pthread_mutex_t native; };
#define GZ_MUTEX_INIT { PTHREAD_MUTEX_INITIALIZER }
void gz_mutex_lock(struct gz_mutex *m);
void gz_mutex_unlock(struct gz_mutex *m);
int gz_sem_create(int count);
int gz_sem_post(int sem);
int gz_sem_wait(int sem, int timeout);
int gz_sem_destroy(int sem);
int gz_thread_create(void (*fn)(void *), void *arg);
uint32_t gz_time_ms(void);
void gz_sleep_ms(uint32_t ms);
int net_resolve(const char *host, uint32_t *addr, int timeout);
#endif
