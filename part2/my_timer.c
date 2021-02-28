#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#define BUFSIZE 100

MODULE_LICENSE("Dual BSD/GPL");

// global variable to track original call time
struct timespec BASE_TIME;

static struct proc_dir_entry *ent;

static char msg[BUFSIZE];
static int len;

static ssize_t myread(struct file *file, char __user *ubuf, size_t count, loff_t *ppos)
{
	struct timespec CURTIME = current_kernel_time();
	struct timespec DIFFERENCE;

	printk(KERN_DEBUG "my_timer read handler\n");

	len += sprintf(msg, "current time: %lu.%lu\n", CURTIME.tv_sec, CURTIME.tv_nsec);

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

	len += sprintf(msg + len, "elapsed time: %lu.%lu\n", DIFFERENCE.tv_sec, DIFFERENCE.tv_nsec);

	if(copy_to_user(ubuf, msg, len))
		return -1;

	*ppos = len;
	BASE_TIME = current_kernel_time();
	return len;
}

static ssize_t mywrite(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
	printk(KERN_DEBUG "mywrite\n");
	if(count > BUFSIZE)
		len = BUFSIZE;
	else
		len = count;
	
	copy_from_user(msg,ubuf,len);
	return len;
}

static struct file_operations myops =
{
	.owner = THIS_MODULE,
	.read = myread,
	.write = mywrite;
};

static int simple_init(void)
{
	ent = proc_create("my_timer", 0666, NULL, &myops);
	if(ent == NULL)
		return -ENOMEM;
	return 0;
}

static void simple_cleanup(void)
{
	proc_remove(ent);
}

module_init(simple_init);
module_exit(simple_cleanup);
