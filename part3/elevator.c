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
#include<linux/slab.h>
MODULE_LICENSE("GPL");

/* syscall stubs */
extern long (*STUB_start_elevator)(void);
extern long (*STUB_issue_request)(int, int, int);
extern long (*STUB_stop_elevator)(void);

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
	int destination; /* destinatin floor */
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

/* Thread parameter */
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
static void elevator_load(int floor, int dir);
static void elevator_unload(int floor);
static long add_passenger(int, int, PassengerType);
static char *state_to_string(State);
static char *passenger_to_string(PassengerType);
static unsigned long make_buffer(void);
static void count_passengers(int *, int *, int *);
static int can_load(int, int, int, Passenger *, int);

/* proc fs file operation (only "read" implemented) */
static const struct file_operations proc_fops = {
 .owner = THIS_MODULE, 
 .read  = proc_read,
};

/* Implementation */

/* runs the elevator thread */
static void thread_init_parameter(struct thread_parameter *parm) {
	static int id = 1;	

	parm->id = id++;	
	parm->kthread = kthread_run(elevator_run, parm, "elevator thread %d", parm->id);
} 

/* to convert elevator state to string */
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

/* to convert passenger type to string */
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

/* make buffer for procfs reading */
static unsigned long make_buffer(void) {
	unsigned long len;
	int i, wolves, sheep, grapes;
	char elevator_sym;
	struct list_head *temp;	
	Passenger *p;

	len = 0;		

	len += sprintf(procfs_buffer + len, "Elevator state: %s\n", 
		state_to_string(elevator.state));

	if (elevator.state != OFFLINE) {
		/* count passengers on the board */
		count_passengers(&wolves, &sheep, &grapes);

		len += sprintf(procfs_buffer + len,
				"Elevator status: %d wolves, %d sheep, %d grapes\n", 
				wolves, sheep, grapes);
		len += sprintf(procfs_buffer + len, 
				"Current floor: %d\n", elevator.current_floor);
		len += sprintf(procfs_buffer + len, 
				"Number of passengers: %d\n", 
				elevator.num_of_passengers);
		len += sprintf(procfs_buffer + len, 
				"Number of passengers waiting: %d\n", 
				elevator.num_waiting);
		len += sprintf(procfs_buffer + len, 
				"Number passengers serviced: %d\n\n", 	
				elevator.num_serviced);

		/* floors */
		for (i = NUM_FLOORS; i >= LOBBY ; --i) {
			if (elevator.current_floor == i) {
				elevator_sym = '*';
			} else {
				elevator_sym = ' ';
			}
			len += sprintf(procfs_buffer + len, 
					"[%c] Floor %d: %d", 
					elevator_sym, i, 
					elevator.floors[i - 1].num_of_passengers);

			/* list all waiting passengers on this floor in FIFO order */
			list_for_each(temp, &elevator.floors[i - 1].list) {
				p = list_entry(temp, Passenger, list);	
				len += sprintf(procfs_buffer + len, " %s", 
				passenger_to_string(p->type));
			} 

			len += sprintf(procfs_buffer + len, "\n");
		}
	}
	
	return len; /* return length of the resulting string */
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
		} else if (p->type == GRAPE) {
			*grapes += 1;
		}
	} 
}

/* procfs read operation */
static ssize_t proc_read(struct file *file, char __user *ubuf, size_t count, 
loff_t *ppos) 
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

	/* copy buffer to user space */
	if(copy_to_user(ubuf, procfs_buffer, procfs_buffer_size)) {
		mutex_unlock(&elevator.mutex);
		return -EFAULT;
	}	
	mutex_unlock(&elevator.mutex);

	*ppos = procfs_buffer_size;
	return procfs_buffer_size;
}

/* returns 1 is elevator is active */
static int is_active(void) {
	if (elevator.state == OFFLINE || elevator.deactivating) {
		return 0;
	}
	return 1;
}

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

	/* init floors */
	for (i = 1; i <= NUM_FLOORS; ++i) {
		elevator.floors[i - 1].num_of_passengers = 0;
		INIT_LIST_HEAD(&elevator.floors[i - 1].list);
	}		

	/* Start elevator thread */
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
	
	/* tell the elevator thread to stop */
	elevator.deactivating = 1;
	kthread_stop(elevator_thread.kthread); 

	/* elevator stopped now */
	elevator.state = OFFLINE;	

	/* Clean up lists*/
	
	/* cleanup floors */
	for (i = 1; i <= NUM_FLOORS; ++i) {		
		list_for_each_safe(temp, dummy, &elevator.floors[i - 1].list) { 
			p = list_entry(temp, Passenger, list);
			list_del(temp);	
			kfree(p);
		} 
	}	

	/* cleanup elevator */
	list_for_each_safe(temp, dummy, &elevator.list) { 
		p = list_entry(temp, Passenger, list);
		list_del(temp);	
		kfree(p);
	} 
}

/* elevator loads passengers on the current floor */
static void elevator_load(int floor, int dir) {
	struct list_head *temp;
	struct list_head *dummy; 
	Passenger *p;
	int wolves, sheep, grapes;	

	/* for each passenger on current floor */
	list_for_each_safe(temp, dummy, &elevator.floors[floor - 1].list) { 		
		p = list_entry(temp, Passenger, list);
		/* count passengers on the board to check loading condition */
		count_passengers(&wolves, &sheep, &grapes);
		/* check if current passenger can be loaded */
		if (can_load(wolves, sheep, grapes, p, dir)) {
			list_del(temp);	/* delete the passenger from the floor */
			/* add the passenger on the board */	
			list_add_tail(&p->list, &elevator.list); 
			/* update statistics */
			elevator.num_of_passengers += 1; 
			elevator.floors[floor - 1].num_of_passengers -= 1;
			elevator.num_waiting -= 1;
		}		
	}
}

/* elevator unloads passengers on the floor */
static void elevator_unload(int floor) {
	struct list_head *temp;
	struct list_head *dummy; 
	Passenger *p;	

	/* for each passenger on the board */
	list_for_each_safe(temp, dummy, &elevator.list) { 
		p = list_entry(temp, Passenger, list);
		/* check if current floor is passenger's destination */
		if (p->destination == floor) {
			/* delete the passenger from the elevator */
			list_del(temp);	
			kfree(p);
			/* update statistics */
			elevator.num_of_passengers -= 1;
			elevator.num_serviced += 1;
		}		
	}
}

/* add passenger at the floor (by issue_request() */
static long add_passenger(int start_floor, int dest_floor, PassengerType type) {
	Passenger *p;
	long result;

	result = 1;

	mutex_lock(&elevator.mutex);

	if (elevator.state != OFFLINE && elevator.deactivating == 0) {
		/* Allocate a new linked list item */
		p = kmalloc(sizeof(Passenger) * 1, __GFP_RECLAIM); 
		if (p == NULL)
			return -ENOMEM;

		p->destination = dest_floor;	
		p->type = type;		
		
		/* insert passenger to the start floor list in FIFO order */	
		list_add_tail(&p->list, &elevator.floors[start_floor - 1].list);
		/* update statistics */
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

	/* Allocate buffer for proc fs */
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
		printk(KERN_ALERT "Elevator: %s: Error: Could not initialize 
			/proc/%s\n", __FUNCTION__, PROC_NAME);
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

	/* deactivate elevator */
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
	err = elevator.state == OFFLINE ? 0 : 1; /* check if elevator already running */
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
	err = (is_active()) ? 0 : 1; /* check if elevator running */
	mutex_unlock(&elevator.mutex);
	
	if (err) {
		return 1;
	}
	/* validate request */
	if (start_floor < LOBBY || start_floor > NUM_FLOORS || 
			destination_floor < LOBBY || destination_floor > NUM_FLOORS || 
			!(type == GRAPE || type == WOLF || type == SHEEP)) {
		return 1;
	}
	/* add passenger at the corresponding floor */
	return add_passenger(start_floor, destination_floor, type);
}

/* Implements stop_elevator() system call */
long my_stop_elevator(void) {
	long err;
    printk(KERN_NOTICE "Elevator: %s\n", __FUNCTION__);

	mutex_lock(&elevator.mutex);
	/* check if elevator already stopped */
	err = (elevator.state == OFFLINE || elevator.deactivating) ? 1 : 0;	
	mutex_unlock(&elevator.mutex);	

	if (!err) {
		elevator_deactivate();		
	}
	return err;    
}

/* elevator stop condition */
static int can_stop(void) {
	return kthread_should_stop() && elevator.num_of_passengers == 0;
}

/* find the nearest floor to which the waiting elevator should move */
static int find_nearest_request(void) {
	int i, elevator_floor, nearest_floor, nearest_distance, distance;
	struct list_head *temp;	
	Passenger *p;	

	elevator_floor = elevator.current_floor;
	nearest_floor = 0;
	nearest_distance = NUM_FLOORS;
	/* for each floor */
	for (i = 1; i <= NUM_FLOORS; ++i) {		
		/* for each passenger at the floor */
		list_for_each(temp, &elevator.floors[i - 1].list) { 
			p = list_entry(temp, Passenger, list);
			/* find distance from elevator to the floor */
			distance = i - elevator_floor;
			if (distance < 0)
				distance = -distance;
			/* select the smallest distance */
			if (nearest_floor == 0 || distance < nearest_distance) {
				nearest_distance = distance;
				nearest_floor = i;
			}
		}
	}	

	return nearest_floor;
}

/*determine if an elevator can take a passenger on board  */
static int can_load(int wolves, int sheep, int grapes, Passenger *p, int dir) {
	if (elevator.num_of_passengers == CAPACITY)
		return 0; /* no room */
	if (p->type == SHEEP && wolves > 0)
		return 0; /* a wolf on the board: a sheep cann't be loaded */
	if (p->type == GRAPE && sheep > 0)
		return 0;  /* a sheep on the board: a grape cann't be loaded */
	/* The elevator does not take on board passengers who need to go 
	the other direction. */
	if (dir == UP && p->destination < elevator.current_floor)
		return 0;
	if (dir == DOWN && p->destination > elevator.current_floor)
		return 0;
	return 1;
}

/* Check if the elevator needs to stop to unload or load passengers.
 This will prevent unnecessary stops.
 However, the elevator may stop but not take passengers 
 if they are incompatible. */
static int need_stop_on_floor(int floor, int direction) {
	int result;
	struct list_head *temp;
	Passenger *p;	

	result = 0;

	mutex_lock(&elevator.mutex);

	/* check passengers on the current floor */
	list_for_each(temp, &elevator.floors[floor - 1].list) { 
		p = list_entry(temp, Passenger, list);
		/* if the elevator goes up, then it picks up only those passengers 
		who need to go up */
		if (direction == UP && p->destination > floor) {
			result = 1;
			break;
		}
		/* if the elevator goes down, then it picks up only those passengers 
		who need to go down */	
		if (direction == DOWN && p->destination < floor) {
			result = 1;
			break;
		}
	} 	

	/* check passengers on the board */
	list_for_each(temp, &elevator.list) { 
		p = list_entry(temp, Passenger, list);	
		if (p->destination == floor) {
			result = 1;
			break;
		}
	} 

	mutex_unlock(&elevator.mutex);

	return result;
}

/* find the topmost floor, to which the elevator moving up should rise. */
static int find_upper_bound(void) {
	int i, upper_floor;
	struct list_head *temp;
	Passenger *p;	

	upper_floor = elevator.current_floor;

	mutex_lock(&elevator.mutex);

	/* check passengers on all floors above the current */
	for (i = elevator.current_floor; i <= NUM_FLOORS; ++i) {	
		/* for each passenger at the floor */	
		list_for_each(temp, &elevator.floors[i - 1].list) { 
			p = list_entry(temp, Passenger, list);
			/* check start floor */
			if (i > upper_floor) {
				upper_floor = i;
			}
			/* check destination floor */
			if (p->destination > upper_floor) {
				upper_floor = p->destination;
			}			
		} 
	}	

	/* check passengers on the board */
	list_for_each(temp, &elevator.list) { 
		p = list_entry(temp, Passenger, list);	
		/* check destination floor */
		if (p->destination > upper_floor) {
			upper_floor = p->destination;
		}	
	} 

	mutex_unlock(&elevator.mutex);

	return upper_floor;
}

/* find the lowest floor, to which the downward elevator should go down. */
static int find_lower_bound(void) {
	int i, lower_floor;
	struct list_head *temp;
	Passenger *p;	

	lower_floor = elevator.current_floor;

	mutex_lock(&elevator.mutex);

	/* check passengers on all floors below the current */
	for (i = elevator.current_floor; i >= LOBBY; --i) {		
		/* for each passenger at the floor */	
		list_for_each(temp, &elevator.floors[i - 1].list) { 			
			p = list_entry(temp, Passenger, list);
			/* check start floor */
			if (i < lower_floor) {
				lower_floor = i;
			}
			/* check destination floor */
			if (p->destination < lower_floor) {
				lower_floor = p->destination;
			}			
		} 
	}	

	/* check passengers on the board */
	list_for_each(temp, &elevator.list) { 
		p = list_entry(temp, Passenger, list);
		/* check destination floor */	
		if (p->destination < lower_floor) {
			lower_floor = p->destination;
		}	
	} 

	mutex_unlock(&elevator.mutex);

	return lower_floor;
}

/* loading passengers operation */
static void loading(int floor, int dir) {	
	/* first unload */
	mutex_lock(&elevator.mutex);			
	elevator.state = LOADING;	
	elevator_unload(floor);		
	mutex_unlock(&elevator.mutex);	

	ssleep(1.0);

	/* then load */
	mutex_lock(&elevator.mutex);
	/* load passengers in the active state only */
	if (is_active()) {
		elevator_load(floor, dir);
	} 
	mutex_unlock(&elevator.mutex);
}

/* waiting for a passenger request while there are no passengers on the 
board or on the floors. */
static int wait_idle(void) {
	int nearest_floor;

	mutex_lock(&elevator.mutex);
	/* it returns 0 if no waiting passengers */
	nearest_floor = find_nearest_request(); 
	/* waits until a waiting passenger appears */
	while (!can_stop() && nearest_floor == 0) {
		elevator.state = IDLE;
		mutex_unlock(&elevator.mutex);
		ssleep(1.0); /* sleep 1 seconds and then check again */
		mutex_lock(&elevator.mutex);
		nearest_floor = find_nearest_request();
	}
	mutex_unlock(&elevator.mutex);
	return can_stop() ? 0 : nearest_floor;
}

/* elevator routine */
static int elevator_run(void *data) {
	int dir, vel, next, curr;
	struct thread_parameter *parm;
	
	parm = data; 

	dir = UP;
	vel = 1;	

	/* starting from IDLE state */

	/* waits until a waiting passenger appears */
	next = wait_idle(); 
	
	while (!can_stop()) {	
		/* the LOOK algorithm */		
		curr = elevator.current_floor;		
		while (!can_stop()) {
			/* check if need stop at the current floor */
			if (need_stop_on_floor(curr, dir)) {
				/* unload and load passengers */
				loading(curr, dir); /* state = LOADING */
				/* recalculate lower or upper bound */
				if (dir == UP) {
					next = find_upper_bound();
				} else {
					next = find_lower_bound();
				}
			}

			mutex_lock(&elevator.mutex);			
			elevator.state = dir; /* state = UP or DOWN */
			mutex_unlock(&elevator.mutex);

			if (curr == next) {
				break; /* change direction needed */
			}			
			
			/* moving */			
			ssleep(2.0);

			mutex_lock(&elevator.mutex);	
			/* update elevator floor */		
			elevator.current_floor += vel;
			curr = elevator.current_floor;
			mutex_unlock(&elevator.mutex);
		}		

		
		if (elevator.num_of_passengers > 0) {
			/* if there are passengers on board */
			
			/* change direction */
			if (dir == UP) {
				dir = DOWN;			
				next = find_lower_bound();
				vel = -1;
			} else {
				dir = UP;			
				next = find_upper_bound();
				vel = 1;
			}
		} else {
			/* if no passengers on the board */

			/* find the nearest request */
			next = wait_idle();
			if (next == 0) {
				break; /* stopped by syscall */
			}
			/* check if change direction needed */
			if (next == elevator.current_floor) {
				next = find_upper_bound();
				if (next == elevator.current_floor) {
					next = find_lower_bound();
				}
			}

			if (dir == UP && next < elevator.current_floor) {
				dir = DOWN;				
				vel = -1;
			} else if (dir == DOWN && next > elevator.current_floor) {
				dir = UP;				
				vel = 1;
			}
		}		
	}	
	return 0;
}
