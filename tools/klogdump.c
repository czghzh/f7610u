/* klogdump - dump the whole kernel ring buffer via syslog(2) READ_ALL.
 * For the stock 4.1.25 firmware, where /proc/kmsg is drained by a daemon
 * and busybox has no dmesg applet. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYSLOG_ACTION_READ_ALL   3
#define SYSLOG_ACTION_SIZE_BUFFER 10

int main(void)
{
	long n, r;
	char *b;

	n = syscall(SYS_syslog, SYSLOG_ACTION_SIZE_BUFFER, NULL, 0);
	if (n <= 0)
		n = 1 << 20;
	b = malloc((size_t)n + 16);
	if (!b)
		return 1;
	r = syscall(SYS_syslog, SYSLOG_ACTION_READ_ALL, b, (int)n);
	if (r < 0) {
		free(b);
		return 2;
	}
	fwrite(b, 1, (size_t)r, stdout);
	fputc('\n', stdout);
	free(b);
	return 0;
}
