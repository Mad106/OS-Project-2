#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#define BUFSIZE 100

// global variable to track original call time
struct timespec BASE_TIME;

static struct proc_dir_entry *ent;

static ssize_t myread(struct file *file, char __user *ubuf, size_t count, loff_t *ppos)
{
	char buf[BUFSIZE];
	int len = 0;

	struct timespec CURTIME = current_kernel_time();
	struct timespec DIFFERENCE;

	printk(KERN_DEBUG "my_timer read handler\n");

	len += sprintf(buf, "current time: %lu.%lu\n", CURTIME.tv_sec, CURTIME.tv_nsec);

	if(*ppos > 0 || count < BUFSIZE)
	{
		BASE_TIME = current_kernel_time();
		return 0;
	}

	// calculate difference
	if(BASE_TIME.tv_nsec < CURTIME.tv_nsec)
	{
		int usec = (CURTIME.tv_nsec - BASE_TIME.tv_nsec) / 1000000 + 1;
		CURTIME.tv_nsec -= 1000000 * usec;
		CURTIME.tv_sec += usec;
	}
	if(BASE_TIME.tv_nsec - CURTIME.tv_nsec > 1000000)
	{
		int usec = (CURTIME.tv_nsec - BASE_TIME.tv_nsec) / 1000000;
		CURTIME.tv_nsec += 1000000 * usec;
		CURTIME.tv_sec -= usec;
	}

	DIFFERENCE.tv_sec = BASE_TIME.tv_sec - CURTIME.tv_sec;
	DIFFERENCE.tv_nsec = BASE_TIME.tv_nsec - CURTIME.tv_nsec;

	len += sprintf(buf + len, "elapsed time: %lu.%lu\n", DIFFERENCE.tv_sec, DIFFERENCE.tv_nsec);

	if(copy_to_user(ubuf, buf, len))
		return -1;

	*ppos = len;
	BASE_TIME = current_kernel_time();
	return len;
}

static struct file_operations myops =
{
	.owner = THIS_MODULE,
	.read = myread,
};

static int simple_init(void)
{
	ent = proc_create("my_timer", 0666, NULL, &myops);
	return 0;
}

static void simple_cleanup(void)
{
	proc_remove(ent);
}

module_init(simple_init);
module_exit(simple_cleanup);
