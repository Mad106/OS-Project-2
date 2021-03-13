#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
//#include <sys/syscall.h>
MODULE_LICENSE("GPL");

/* syscall numbers */
#define __NR_START_ELEVATOR 335
#define __NR_STOP_ELEVATOR 336
#define __NR_ISSUE_REQUEST 337

/* proc fs */
#define PROC_NAME "elevator"
#define PROC_SIZE 4096
#define PROC_PERMS 0644
#define PROC_PARENT NULL

/**
 * Holds information about the proc fs file
 *
 */
static struct proc_dir_entry *proc_file;

/**
 * The buffer used with proc fs
 *
 */
static char *procfs_buffer;

/* Elevator constants */
#define CAPACITY 10
#define NUM_FLOORS 10
#define LOBBY 1

typedef enum {OFFLINE, IDLE, LOADING, UP, DOWN } State;
typedef enum {WOLF = 2, SHEEP = 1, GRAPE = 0} PassengerType;

/* Passenger list item */
typedef struct Passenger {
	int destination;
	PassengerType type;
	struct list_head list; 
} Passenger;

/* Floor list of passengers */
typedef struct Floor {
	int num_of_passengers;
	struct list_head list; 
} Floor;

/* Elevator */
typedef struct Elevator {	
	State state;	
	int current_floor;
	int num_of_passengers;
	int num_waiting;
	int num_serviced;
	int deactivating;
	struct list_head list; /* passengers on the board */
	struct mutex mutex;

	Floor floors[NUM_FLOORS];
} Elevator;

static Elevator elevator;

/* Thread */
struct thread_parameter {	
	int id;
	struct task_struct *kthread;	
};

static struct thread_parameter elevator_thread;

/* Prototypes */

long my_start_elevator(void);
long my_issue_request(int, int, int);
long my_stop_elevator(void);

static ssize_t proc_read(struct file *, char __user *, size_t, loff_t *);
static int elevator_activate(void);
static void elevator_deactivate(void);
static int elevator_run(void *);
static void thread_init_parameter(struct thread_parameter *);
static void elevator_load(int floor);
static void elevator_unload(int floor);
static long add_passenger(int, int, PassengerType);
static char *state_to_string(State);
static char *passenger_to_string(PassengerType);
static unsigned long make_buffer(void);
static void count_passengers(int *wolves, int *sheep, int *grapes);

static const struct file_operations proc_fops = {
 .owner = THIS_MODULE, 
 .read  = proc_read,
};

/* Implementation */

static void thread_init_parameter(struct thread_parameter *parm) {
	static int id = 1;	

	parm->id = id++;	
	parm->kthread = kthread_run(elevator_run, parm, "elevator thread %d", parm->id);
} 

static char *state_to_string(State s) {
	static char state_buffer[8];
	if (s == OFFLINE) {
		strcpy(state_buffer, "OFFLINE");
	} else if (elevator.state == IDLE) {
		strcpy(state_buffer, "IDLE");
	} else if (elevator.state == LOADING) {
		strcpy(state_buffer, "LOADING");
	} else if (elevator.state == UP) {
		strcpy(state_buffer, "UP");
	} else if (elevator.state == DOWN) {
		strcpy(state_buffer, "DOWN");
	}
	return state_buffer;
}

static char *passenger_to_string(PassengerType p) {
	static char passenger_buffer[2];
	if (p == WOLF) {
		strcpy(passenger_buffer, "W");
	} else if (p == SHEEP) {
		strcpy(passenger_buffer, "S");
	} else if (p == GRAPE) {
		strcpy(passenger_buffer, "G");
	} 
	return passenger_buffer;
}

static unsigned long make_buffer(void) {
	unsigned long len = 0;
	int i, wolves, sheep, grapes;
	char elevator_sym;
	struct list_head *temp;
	Passenger *p;

	len += sprintf(procfs_buffer + len, "Elevator state: %s\n", state_to_string(elevator.state));

	if (elevator.state != OFFLINE) {
		/* count passengers on the board */
		count_passengers(&wolves, &sheep, &grapes);

		len += sprintf(procfs_buffer + len,
				"Elevator status: %d wolves, %d sheep, %d grapes\n", 
				wolves, sheep, grapes);
		len += sprintf(procfs_buffer + len, 
				"Current floor: %d\n", elevator.current_floor);
		len += sprintf(procfs_buffer + len, 
				"Number of passengers: %d\n", elevator.num_of_passengers);
		len += sprintf(procfs_buffer + len, 
				"Number of passengers waiting: %d\n", elevator.num_waiting);
		len += sprintf(procfs_buffer + len, 
				"Number passengers serviced: %d\n\n", elevator.num_serviced);

		/* floors */
		for (i = NUM_FLOORS; i >= LOBBY ; --i) {
			if (elevator.current_floor == i) {
				elevator_sym = '*';
			} else {
				elevator_sym = ' ';
			}
			len += sprintf(procfs_buffer + len, 
					"[%c] Floor %d: %d", 
					elevator_sym, i, elevator.floors[i - 1].num_of_passengers);

			list_for_each(temp, &elevator.floors[i - 1].list) {
				p = list_entry(temp, Passenger, list);
				len += sprintf(procfs_buffer + len, " %s", passenger_to_string(p->type));
			}

			len += sprintf(procfs_buffer + len, "\n");
		}
	}

	return len;
}

/* count passengers on the board */
static void count_passengers(int *wolves, int *sheep, int *grapes) {
	struct list_head *temp;
	Passenger *p;

	*wolves = *sheep = *grapes = 0;
	list_for_each(temp, &elevator.list) {
		p = list_entry(temp, Passenger, list);
		if (p->type == WOLF) {
			*wolves += 1;
		} else if (p->type == SHEEP) {
			*sheep += 1;
		} if (p->type == GRAPE) {
			*grapes += 1;
		}
	} 
}

/*  */
static ssize_t proc_read(struct file *file, char __user *ubuf, size_t count, loff_t *ppos) 
{
	unsigned long procfs_buffer_size = 0;

	printk(KERN_NOTICE "Elevator: %s\n", __FUNCTION__);

	mutex_lock(&elevator.mutex);

	if(*ppos == 0) {
		procfs_buffer_size = make_buffer();
	}

	if (*ppos > 0 || count < procfs_buffer_size) {
		mutex_unlock(&elevator.mutex);
		return 0;
	}

	if(copy_to_user(ubuf, procfs_buffer, procfs_buffer_size)) {
		mutex_unlock(&elevator.mutex);
		return -EFAULT;
	}	
	mutex_unlock(&elevator.mutex);

	*ppos = procfs_buffer_size;
	return procfs_buffer_size;
}

/* System call stub */
/*long (*STUB_start_elevator)(void) = NULL;
EXPORT_SYMBOL(STUB_start_elevator);

long (*STUB_issue_request)(int, int, int) = NULL;
EXPORT_SYMBOL(STUB_issue_request);

long (*STUB_stop_elevator)(void) = NULL;
EXPORT_SYMBOL(STUB_stop_elevator);*/

/* System call wrapper */
/*SYSCALL_DEFINE0(start_elevator) {
        if (STUB_start_elevator != NULL)
                return STUB_start_elevator();
        else
                return -ENOSYS;
}

SYSCALL_DEFINE3(issue_request, int, start_floor, int, destination_floor, int, type) {
        if (STUB_issue_request != NULL)
                return STUB_issue_request(start_floor, destination_floor, type);
        else
                return -ENOSYS;
}

SYSCALL_DEFINE0(stop_elevator) {
        if (STUB_stop_elevator != NULL)
                return STUB_stop_elevator();
        else
                return -ENOSYS;
}*/


/* activates the elevator on start_elevator() syscall */
static int elevator_activate(void) {
	int i, err;

	elevator.state = IDLE;
	elevator.current_floor = LOBBY;
	elevator.num_of_passengers = 0;
	elevator.num_waiting = 0;
	elevator.num_serviced = 0;
	elevator.deactivating = 0;

	INIT_LIST_HEAD(&elevator.list);

	for (i = 1; i <= NUM_FLOORS; ++i) {
		elevator.floors[i - 1].num_of_passengers = 0;
		INIT_LIST_HEAD(&elevator.floors[i - 1].list);
	}

	/* Thread */
	thread_init_parameter(&elevator_thread);
	if (IS_ERR(elevator_thread.kthread)) {
		printk(KERN_WARNING "error spawning thread");
		err = PTR_ERR(elevator_thread.kthread);
	} else {
		err = 0;
	}

	return err;
}

/* deactivates the elevator on stop_elevator() syscall */
static void elevator_deactivate(void) {
	int i;
	struct list_head *temp;
	struct list_head *dummy; 
	Passenger *p;	

	mutex_lock(&elevator.mutex);
	elevator.deactivating = 1;
	mutex_unlock(&elevator.mutex);

	kthread_stop(elevator_thread.kthread); 

	elevator.state = OFFLINE;	
	elevator.deactivating = 0; /* deactivated */

	/* Clean up lists*/
	for (i = 1; i <= NUM_FLOORS; ++i) {		
		list_for_each_safe(temp, dummy, &elevator.floors[i - 1].list) { 
			p = list_entry(temp, Passenger, list);
			list_del(temp);	
			kfree(p);
		} 
	}	

	list_for_each_safe(temp, dummy, &elevator.list) { 
		p = list_entry(temp, Passenger, list);
		list_del(temp);	
		kfree(p);
	} 
}

static int can_load(int wolves, int sheep, int grapes, PassengerType type) {
	if (elevator.num_of_passengers == CAPACITY)
		return 0;
	if (type == sheep && wolves > 0)
		return 0;
	if (type == grapes && sheep > 0)
		return 0;
	return 1;
}

static void elevator_load(int floor) {
	struct list_head *temp;
	struct list_head *dummy; 
	Passenger *p;
	int wolves, sheep, grapes;

	count_passengers(&wolves, &sheep, &grapes);

	list_for_each_safe(temp, dummy, &elevator.floors[floor - 1].list) { 
		p = list_entry(temp, Passenger, list);
		if (can_load(wolves, sheep, grapes, p->type)) {
			list_del(temp);			
			list_add_tail(&p->list, &elevator.list); /* insert at back of list */	
			elevator.num_of_passengers += 1; 
			elevator.floors[floor - 1].num_of_passengers -= 1;
			elevator.num_waiting -= 1;
		}		
	}
}

static void elevator_unload(int floor) {
	struct list_head *temp;
	struct list_head *dummy; 
	Passenger *p;	

	list_for_each_safe(temp, dummy, &elevator.list) { 
		p = list_entry(temp, Passenger, list);
		if (p->destination == floor) {
			list_del(temp);	
			kfree(p);
			elevator.num_of_passengers -= 1;
			elevator.num_serviced += 1;
		}		
	}
}

static long add_passenger(int start_floor, int dest_floor, PassengerType type) {
	Passenger *p;
	long result;

	result = 1;

	mutex_lock(&elevator.mutex);

	if (elevator.state != OFFLINE && elevator.deactivating == 0) {
		p = kmalloc(sizeof(Passenger) * 1, __GFP_RECLAIM); 
		if (p == NULL)
			return -ENOMEM;

		p->destination = dest_floor;	
		p->type = type;		
		
		list_add_tail(&p->list, &elevator.floors[start_floor - 1].list); /* insert at back of list */	
		
		elevator.floors[start_floor - 1].num_of_passengers += 1; 
		elevator.num_waiting += 1;

		result = 0;
	}

	mutex_unlock(&elevator.mutex);

	return result;
}

/* Initialization and clean-up */

/* Module initialization */
static int elevator_init(void) {  

	procfs_buffer = kmalloc(PROC_SIZE, __GFP_RECLAIM); 
	if (procfs_buffer == NULL) {
		return -ENOMEM;
	}

	/* Initialize elevator */
	elevator.state = OFFLINE;	
	mutex_init(&elevator.mutex);

	/* Initializing proc fs */    
	proc_file = proc_create(PROC_NAME, PROC_PERMS, PROC_PARENT, &proc_fops);
	if (proc_file == NULL) {
		printk(KERN_ALERT "Elevator: %s: Error: Could not initialize /proc/%s\n", __FUNCTION__, PROC_NAME);
		remove_proc_entry(PROC_NAME, PROC_PARENT); 
		return -ENOMEM;
	}
	printk(KERN_INFO "Elevator: %s: /proc/%s created\n", __FUNCTION__, PROC_NAME);		

	STUB_start_elevator = my_start_elevator;
	STUB_issue_request = my_issue_request;
	STUB_stop_elevator = my_stop_elevator;

	return 0;
}
module_init(elevator_init);

/* Module exiting */
static void elevator_exit(void) {
    STUB_start_elevator = NULL;
    STUB_issue_request = NULL;
    STUB_stop_elevator = NULL;

    /* Cleaning proc fs */
    remove_proc_entry(PROC_NAME, NULL);
	printk(KERN_INFO "Elevator: %s: /proc/%s removed\n",  __FUNCTION__, PROC_NAME);

	/* clean-up lists */
	if (elevator.state != OFFLINE) {
		elevator_deactivate();
	}

	mutex_destroy(&elevator.mutex);
	kfree(procfs_buffer);
}
module_exit(elevator_exit);

/* Implements start_elevator() system call */
long my_start_elevator(void) {
	int err;

    	printk(KERN_NOTICE "Elevator: %s\n", __FUNCTION__);

	mutex_lock(&elevator.mutex);	
	err = elevator.state == OFFLINE ? 0 : 1;	
	mutex_unlock(&elevator.mutex);

	if (!err) {
		err = elevator_activate();
	}

	return err;
}

/* Implements issue_request() system call */
long my_issue_request(int start_floor, int destination_floor, int type) {
    int err;

	printk(KERN_NOTICE "Elevator: %s\n", __FUNCTION__);

	mutex_lock(&elevator.mutex);
	err = (elevator.state == OFFLINE || elevator.deactivating) ? 1 : 0;
	mutex_unlock(&elevator.mutex);
	
	if (err) {
		return 1;
	}

	if (start_floor < LOBBY || start_floor > NUM_FLOORS || 
			destination_floor < LOBBY || destination_floor > NUM_FLOORS || 
			!(type == GRAPE || type == WOLF || type == SHEEP)) {
		return 1;
	}

	return add_passenger(start_floor, destination_floor, type);
}

/* Implements stop_elevator() system call */
long my_stop_elevator(void) {
	long err;
    printk(KERN_NOTICE "Elevator: %s\n", __FUNCTION__);

	mutex_lock(&elevator.mutex);
	err = (elevator.state == OFFLINE || elevator.deactivating) ? 1 : 0;	
	mutex_unlock(&elevator.mutex);	

	if (!err) {
		elevator_deactivate();		
	}
	return err;    
}

static int can_stop(void) {
	return kthread_should_stop() && elevator.num_of_passengers == 0;
}

static int elevator_run(void *data) {
	int i, dir, first, last, vel;
	struct thread_parameter *parm;
	
	parm = data; 

	dir = UP;
	first = LOBBY;
	last = NUM_FLOORS;
	vel = 1;

	while (!can_stop()) {
		for (i = first; i != last + vel && !can_stop();  i += vel) {	
			/* loading */
			mutex_lock(&elevator.mutex);			
			elevator.state = LOADING;
			elevator.current_floor = i;
			elevator_unload(i);		
			mutex_unlock(&elevator.mutex);	

			ssleep(1.0);

			mutex_lock(&elevator.mutex);
			if (!elevator.deactivating) {
				elevator_load(i);
			} 
			elevator.state = dir;
			mutex_unlock(&elevator.mutex);
			
			/* moving */			
			ssleep(2.0);
		}

		if (dir == UP) {
			dir = DOWN;
			first = NUM_FLOORS;
			last = LOBBY;
			vel = -1;
		} else {
			dir = UP;
			first = LOBBY;
			last = NUM_FLOORS;
			vel = 1;
		}
	}	
	return 0;
}
