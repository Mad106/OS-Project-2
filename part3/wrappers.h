#ifndef __WRAPPERS_H
#define __WRAPPERS_H

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

#define __NR_START_ELEVATOR 335
#define __NR_STOP_ELEVATOR 336
#define __nR_ISSUE_REQUEST 337

int start_elevator() {
	return syscall(__NR_START_ELEVATOR);
}

int issue_request(int start, int dest, int type) {
	return syscall(__NR_ISSUE_REQUEST, start, dest, type);
}

int stop_elevator() {
	return syscall(__NR_STOP_ELEVATOR);
}

#endif
