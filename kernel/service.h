#ifndef GLAZE_KERNEL_SERVICE_H
#define GLAZE_KERNEL_SERVICE_H
#include "proc.h"
void services_init(void);
int sys_services(struct proc *p, uint32_t output, uint32_t size);
int sys_service_set(struct proc *p, uint32_t name, int enabled);
#endif
