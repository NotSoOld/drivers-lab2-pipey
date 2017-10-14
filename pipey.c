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
#include <linux/string.h>

#define CBUFFERSIZE 65536
#define DEVICES_COUNT 1
#define PIPEY_MAJOR 200
#define PIPEY_MINOR 0

static int reader_handle;
static int writer_handle;
static char *cyclic_buffer = NULL;
static wait_queue_head_t queue;
static int buffer_not_empty = 0;
static int is_eof = 0;
static int exclusive_read = 0;
struct cdev pipey_cdev;
static dev_t dev;
const struct file_operations pipey_file_ops;


static int pipey_open(struct inode *i, struct file *f) 
{
	f->private_data = &(get_current_user()->uid.val);
	return 0;
}


//int pipey_release(struct inode *i, struct file *f) {}


static long pipey_ioctl(struct file *f, unsigned int cmd, unsigned long arg) 
{
	switch (cmd) {
		case 0x10:
			// EOF
			is_eof = 1;
			break;
		case 0x30:
			// Exclusive read mode
			exclusive_read = 1;
			break;
		case 0x31:
			// Normal read mode
			exclusive_read = 0;
			break;
		default:
			printk("ERROR! Unrecognizable code 0x%x sent to ioctl\n", cmd);
			return -1;
	}
	return 0;
}


static ssize_t pipey_read(struct file *f, char __user *buf, size_t sz, loff_t *off)
{
	// Writes into buf! (user space app READS us)
	if (exclusive_read && (*(uid_t *)(f->private_data)) != get_current_user()->uid.val) {
		printk("Exclusive read mode - reading not permitted for you.\n");
		return -1;
	}
	
	unsigned long copied = 0;
	ssize_t written = 0;
	unsigned long available_bytes = 0;
	
	while (true) {
		if (reader_handle < writer_handle) {
			available_bytes = writer_handle - reader_handle;
			if (sz < available_bytes) {
				if (copy_to_user(buf, cyclic_buffer + reader_handle, sz)) {
					printk("Error! Some bytes cannot be copied to user.\n");
					return -1;
				}
				written += sz;
				reader_handle += sz;
				sz = 0;
			}
			else {
				if (copy_to_user(buf, cyclic_buffer + reader_handle, available_bytes)) {
					printk("Error! Some bytes cannot be copied to user.\n");
					return -1;
				}
				written += available_bytes;
				reader_handle += available_bytes;
				sz -= available_bytes;
			}
		}
		else if ((reader_handle > writer_handle) || (writer_handle == reader_handle && buffer_not_empty == 1)) {
			available_bytes = CBUFFERSIZE - reader_handle;
			if (sz < available_bytes) {
				if (copy_to_user(buf, cyclic_buffer + reader_handle, sz)) {
					printk("Error! Some bytes cannot be copied to user.\n");
					return -1;
				}
				written += sz;
				reader_handle += sz;
				sz = 0;
			}
			else {
				if (copy_to_user(buf, cyclic_buffer + reader_handle, available_bytes)) {
					printk("Error! Some bytes cannot be copied to user.\n");
					return -1;
				}
				written += available_bytes;
				reader_handle = 0;
				sz -= available_bytes;
				copied = sz <= writer_handle ? sz : writer_handle;
				if (copy_to_user(buf, cyclic_buffer, copied)) {
					printk("Error! Some bytes cannot be copied to user.\n");
					return -1;
				}
				sz -= copied;
				written += copied;
				reader_handle = copied;
			}
		}
		// Else - buffer is empty, wait for some data to arrive.
		buffer_not_empty = 0;
		
		if (sz == 0) {
			return written;
		}
		else {
			if (is_eof) {
				// This is the end of file, no more info will come, just return.
				is_eof = 0;
				return written;
			}
			// Wait for new information to arrive.
			wake_up_interruptible(&queue);
			wait_event_interruptible(queue, buffer_not_empty == 1);
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
		if (writer_handle < reader_handle) {
			available_bytes = reader_handle - writer_handle;
			if (sz < available_bytes) {
				if (copy_from_user((void *)buf, cyclic_buffer + writer_handle, sz)) {
					printk("Error! Some bytes cannot be copied from user.\n");
					return -1;
				}
				readed += sz;
				writer_handle += sz;
				sz = 0;
			}
			else {
				if (copy_from_user((void *)buf, cyclic_buffer + writer_handle, available_bytes)) {
					printk("Error! Some bytes cannot be copied from user.\n");
					return -1;
				}
				readed += available_bytes;
				writer_handle += available_bytes;
				sz -= available_bytes;
			}
		}
		else if ((writer_handle > reader_handle) || (writer_handle == reader_handle && buffer_not_empty == 0)) {
			available_bytes = CBUFFERSIZE - writer_handle;
			if (sz < available_bytes) {
				if (copy_from_user((void *)buf, cyclic_buffer + writer_handle, sz)) {
					printk("Error! Some bytes cannot be copied from user.\n");
					return -1;
				}
				readed += sz;
				writer_handle += sz;
				sz = 0;
			}
			else {
				if (copy_from_user((void *)buf, cyclic_buffer + writer_handle, available_bytes)) {
					printk("Error! Some bytes cannot be copied from user.\n");
					return -1;
				}
				readed += available_bytes;
				writer_handle = 0;
				sz -= available_bytes;
				copied = sz <= reader_handle ? sz : reader_handle;
				if (copy_from_user((void *)buf, cyclic_buffer, copied)) {
					printk("Error! Some bytes cannot be copied from user.\n");
					return -1;
				}
				sz -= copied;
				readed += copied;
				writer_handle = copied;
			}
		}
		// Else - buffer is full, wait for some reading by userspace app.
		buffer_not_empty = 1;
		
		if (sz == 0) {
			return readed;
		}
		else {
			// Wait for new information to depart (to free some buffer space).
			wake_up_interruptible(&queue);
			wait_event_interruptible(queue, buffer_not_empty == 0);
		}
	}
}


static char *init_cyclic_buffer(void)
{
	char *cyclic_buffer = (char *)kzalloc(CBUFFERSIZE * sizeof(char), GFP_USER);
	if (cyclic_buffer == NULL) {
		printk("ERROR!! Failed to allocate memory for cyclic buffer.\n");
		return NULL;
	}
	reader_handle = 0;
	writer_handle = 0;
	return cyclic_buffer;
}


static int __init pipey_init(void)
{
	cyclic_buffer = init_cyclic_buffer();
	if (!cyclic_buffer) {
		return -1;
	}
	
	dev = MKDEV(PIPEY_MAJOR, PIPEY_MINOR);
	if (register_chrdev_region(dev, DEVICES_COUNT, "pipey")) {
		printk("ERROR!! Failed to register pipey device.\n");
		return -2;
	}
	cdev_init(&pipey_cdev, &pipey_file_ops);
	if (cdev_add(&pipey_cdev, dev, DEVICES_COUNT)) {
		printk("ERROR!! Failed to add pipey device as char device.\n");
		return -3;
	}
	init_waitqueue_head(&queue);
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
	//.release = pipey_release,
	.unlocked_ioctl = pipey_ioctl,
	.write = pipey_write,
	.read = pipey_read
};

module_init(pipey_init);
module_exit(pipey_exit);
