#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#define BUFSIZE 100

MODULE_LICENSE("GPL");

// global variable to track original call time
struct timespec BASE_TIME;
static struct proc_dir_entry * ent;
static char buf[BUFSIZE];
static int firstRun = 1;
static int len = 0;	//length of message

static ssize_t myread(struct file *file, char __user *ubuf, size_t count, loff_t *ppos)
{
	struct timespec CURTIME = current_kernel_time();
	struct timespec DIFFERENCE;
	static int finished = 0;
	len = 0;
	// prevents infinite loop
	if(finished)
	{
		finished = 0;
		return 0;
	}
	finished = 1;
	
	len += sprintf(buf, "current time: %lu.%lu\n", CURTIME.tv_sec,
		 CURTIME.tv_nsec);

	if(firstRun == 0)	//there is something to compare current to
	{
		// calculate difference
		DIFFERENCE.tv_sec = CURTIME.tv_sec - BASE_TIME.tv_sec;
		DIFFERENCE.tv_nsec = CURTIME.tv_nsec - BASE_TIME.tv_nsec;
		if(DIFFERENCE.tv_nsec < 0)
		{
			--DIFFERENCE.tv_sec;
			DIFFERENCE.tv_nsec += 1000000000L;
		}
		len += sprintf(buf + len, "elapsed time: %lu.%lu\n",
			DIFFERENCE.tv_sec, DIFFERENCE.tv_nsec);
	}

	if(copy_to_user(ubuf, buf, len))
		return -EFAULT;
	BASE_TIME = current_kernel_time();
	firstRun = 0;
	*ppos = len;
	return len;
}

static ssize_t mywrite(struct file * file, const char __user *ubuf, size_t count, 
loff_t *ppos)
{
	printk(KERN_DEBUG "mywrite\n");
	if(count > BUFSIZE)
		len = BUFSIZE;
	else
		len = count;

	copy_from_user(buf, ubuf, len);
	return len;
}

static struct file_operations myops =
{
	.owner = THIS_MODULE,
	.read = myread,
	.write = mywrite,
};

static int simple_init(void)
{
	ent = proc_create("my_timer", 0666, NULL, &myops);
	if(ent == NULL)	//there needs to be reference to my_timer
		return -ENOMEM;
	return 0;
}

static void simple_cleanup(void)
{
	proc_remove(ent);
}

module_init(simple_init);
module_exit(simple_cleanup);
