#include <linux/init.h>    // magic
#include <linux/module.h>  // magic #2
#include <linux/kernel.h>  // magic #3
#include <linux/gfp.h>     // kzalloc flags
#include <linux/slab.h>    // kzalloc
#include <linux/uaccess.h> // copy_from/to_user
#include <linux/wait.h>    // wait_queue
#include <linux/fs.h>      // file_operations
#include <linux/kdev_t.h>  // mkdev
#include <linux/cdev.h>    // cdev_init
#include <linux/cred.h>    // get_current_user
#include <linux/mutex.h>
#include "pipey_ioctl.h"

#define CBUFFERSIZE 65536
#define DEVICES_COUNT 1
#define PIPEY_MAJOR 200
#define PIPEY_MINOR 0

MODULE_LICENSE("GPL");
static int reader_handle;
static int writer_handle;
static char *cyclic_buffer;
static wait_queue_head_t reader_queue;
static wait_queue_head_t writer_queue;
static int buffer_condition = -1; // -1 - empty, 0 - partially filled, 1 - full
static int is_eof;
static int opened_descriptors;
static int exclusive_read;
struct cdev pipey_cdev;
static dev_t dev;
const struct file_operations pipey_file_ops;
static uid_t writing_user_id;
static struct mutex *pipey_mutex;


static char *init_cyclic_buffer(void)
{
	char *buffer = (char *)kzalloc(CBUFFERSIZE * sizeof(char), GFP_USER);
	
	if (buffer == NULL) {
		printk(KERN_ERR "ERROR!! Failed to allocate memory for cyclic buffer.\n");
		return NULL;
	}
	reader_handle = 0;
	writer_handle = 0;
	is_eof = 0;
	writing_user_id = 0;
	buffer_condition = -1;
	return buffer;
}


static int pipey_open(struct inode *i, struct file *f) 
{
	opened_descriptors++;
	if (opened_descriptors > 1) {
		is_eof = 0;
	}
	f->private_data = &(current_uid().val);
	printk(KERN_INFO "Reg fd %d, %d\n", *(uid_t *)(f->private_data), current_uid().val);
	return 0;
}


static int pipey_release(struct inode *i, struct file *f)
{
	opened_descriptors--;
	if (opened_descriptors <= 1) {
		is_eof = 1;
		wake_up_interruptible(&reader_queue);
	}
	return 0;
}


static long pipey_ioctl(struct file *f, unsigned int cmd, unsigned long arg) 
{
	switch (cmd) {
	case PIPEY_SET_EXCL_READ:
		exclusive_read = 1;
		writing_user_id = *(uid_t *)(f->private_data);
		printk(KERN_INFO "Set user id %d\n", writing_user_id);
		break;
	case PIPEY_SET_NORM_READ:
		exclusive_read = 0;
		writing_user_id = 0;
		break;
	default:
		printk(KERN_ERR "ERROR! Unrecognizable code 0x%x sent to ioctl\n", cmd);
		return -1;
	}
	return 0;
}


static ssize_t pipey_read(struct file *f, char __user *buf, size_t sz, loff_t *off)
{
	unsigned long copied = 0;
	ssize_t written = 0;
	unsigned long available_bytes = 0;
	
	// Writes into buf! (user space app READS us)
	if (exclusive_read && writing_user_id != current_uid().val) {
		printk(KERN_WARNING "Exclusive read mode - reading not permitted for you.\n");
		printk(KERN_WARNING "(you: %d, host: %d)\n", current_uid().val, writing_user_id);
		return -1;
	}
	
	while (true) {
		mutex_lock(pipey_mutex);
		if (reader_handle < writer_handle) {
			available_bytes = writer_handle - reader_handle;
			if (sz < available_bytes) {
				if (copy_to_user(buf+written, cyclic_buffer + reader_handle, sz)) {
					printk(KERN_ERR "Error! Some bytes cannot be copied to user.\n");
					return -1;
				}
				written += sz;
				reader_handle += sz;
				sz = 0;
				buffer_condition = 0;
			} else {
				if (copy_to_user(buf+written, cyclic_buffer + reader_handle, available_bytes)) {
					printk(KERN_ERR "Error! Some bytes cannot be copied to user.\n");
					return -1;
				}
				written += available_bytes;
				reader_handle += available_bytes;
				sz -= available_bytes;
				buffer_condition = -1;
			}
		} else if ((reader_handle > writer_handle) || 
		        (writer_handle == reader_handle && buffer_condition == 1)) {
			available_bytes = CBUFFERSIZE - reader_handle;
			if (sz < available_bytes) {
				if (copy_to_user(buf+written, cyclic_buffer + reader_handle, sz)) {
					printk(KERN_ERR "Error! some bytes cannot be copied to user.\n");
					return -1;
				}
				written += sz;
				reader_handle += sz;
				sz = 0;
				buffer_condition = 0;
			} else {
				if (copy_to_user(buf+written, cyclic_buffer + reader_handle, available_bytes)) {
					printk(KERN_ERR "Error! Some bytes cannot be copied to user.\n");
					return -1;
				}
				written += available_bytes;
				reader_handle = 0;
				sz -= available_bytes;
				copied = sz <= writer_handle ? sz : writer_handle;
				if (copy_to_user(buf+written, cyclic_buffer, copied)) {
					printk(KERN_ERR "Error! Some bytes cannot be copied to user.\n");
					return -1;
				}
				sz -= copied;
				written += copied;
				reader_handle = copied;
				if (copied == writer_handle) {
					buffer_condition = -1;
				} else {
					buffer_condition = 0;
				}
			}
		}
		mutex_unlock(pipey_mutex);
		// Else - buffer is empty, wait for some data to arrive.
		printk(KERN_DEBUG "Sent %d bytes, sz = %d, eof = %d\n", written, sz, is_eof);
		if (sz == 0 || (is_eof && buffer_condition == -1)) {
			return written;
		} else {
			// Wait for new information to arrive.
			wake_up_interruptible(&writer_queue);
			wait_event_interruptible(reader_queue, (buffer_condition >= 0));
		}
	}
}


static ssize_t pipey_write(struct file *f, const char __user *buf, size_t sz, loff_t *off)
{
	// Writes cz bytes to device from buf (user space app WRITES to us)
	unsigned long copied = 0;
	ssize_t readed = 0;
	unsigned long available_bytes = 0;
	
	while (true) {
		mutex_lock(pipey_mutex);
		if (writer_handle < reader_handle) {
			available_bytes = reader_handle - writer_handle;
			if (sz < available_bytes) {
				if (copy_from_user(cyclic_buffer + writer_handle, buf+readed, sz)) {
					printk(KERN_ERR "Error! Some bytes cannot be copied from user.\n");
					return -1;
				}
				readed += sz;
				writer_handle += sz;
				sz = 0;
				buffer_condition = 0;
			} else {
				if (copy_from_user(cyclic_buffer + writer_handle, buf+readed, available_bytes)) {
					printk(KERN_ERR "Error! Some bytes cannot be copied from user.\n");
					return -1;
				}
				readed += available_bytes;
				writer_handle += available_bytes;
				sz -= available_bytes;
				buffer_condition = 1;
			}
		} else if ((writer_handle > reader_handle) || 
		        (writer_handle == reader_handle && buffer_condition == -1)) {
			available_bytes = CBUFFERSIZE - writer_handle;
			if (sz < available_bytes) {
				if (copy_from_user(cyclic_buffer + writer_handle, buf+readed, sz)) {
					printk("Error! Some bytes cannot be copied from user.\n");
					return -1;
				}
				readed += sz;
				writer_handle += sz;
				sz = 0;
				buffer_condition = 0;
			} else {
				if (copy_from_user(cyclic_buffer + writer_handle, buf+readed, available_bytes)) {
					printk(KERN_ERR "Error! Some bytes cannot be copied from user.\n");
					return -1;
				}
				readed += available_bytes;
				writer_handle = 0;
				sz -= available_bytes;
				copied = sz <= reader_handle ? sz : reader_handle;
				if (copy_from_user(cyclic_buffer, buf+readed, copied)) {
					printk(KERN_ERR "Error! Some bytes cannot be copied from user.\n");
					return -1;
				}
				sz -= copied;
				readed += copied;
				writer_handle = copied;
				if (copied == reader_handle) {
					buffer_condition = 1;
				} else {
					buffer_condition = 0;
				}
			}
		}
		mutex_unlock(pipey_mutex);
		// Else - buffer is full, wait for some reading by userspace app.
		printk(KERN_DEBUG "Received %d bytes\n", readed);
		if (sz == 0) {
			return readed;
		} else {
			// Wait for new information to depart (to free some buffer space).
			wake_up_interruptible(&reader_queue);
			wait_event_interruptible(writer_queue, (buffer_condition <= 0));
		}
	}
}


static int __init pipey_init(void)
{	
	dev = MKDEV(PIPEY_MAJOR, PIPEY_MINOR);
	if (register_chrdev_region(dev, DEVICES_COUNT, "pipey")) {
		printk(KERN_CRIT "ERROR!! Failed to register pipey device.\n");
		return -2;
	}
	cdev_init(&pipey_cdev, &pipey_file_ops);
	if (cdev_add(&pipey_cdev, dev, DEVICES_COUNT)) {
		printk(KERN_CRIT "ERROR!! Failed to add pipey device as char device.\n");
		return -3;
	}
	init_waitqueue_head(&reader_queue);
	init_waitqueue_head(&writer_queue);
	cyclic_buffer = init_cyclic_buffer();
	if (!cyclic_buffer) {
		printk(KERN_CRIT "ERROR!! Failed to initialize cyclic buffer\n");
		return -1;
	}
	opened_descriptors = 0;
	pipey_mutex = (struct mutex *)kzalloc(sizeof(struct mutex), GFP_KERNEL);
	mutex_init(pipey_mutex);
	return 0;
} 


static void __exit pipey_exit(void)
{
	kfree(cyclic_buffer);
	dev = MKDEV(PIPEY_MAJOR, PIPEY_MINOR);
	unregister_chrdev_region(dev, DEVICES_COUNT);
	cdev_del(&pipey_cdev);
}


const struct file_operations pipey_file_ops = {
	.owner = THIS_MODULE,
	.open = pipey_open,
	.release = pipey_release,
	.unlocked_ioctl = pipey_ioctl,
	.write = pipey_write,
	.read = pipey_read
};

module_init(pipey_init);
module_exit(pipey_exit);
