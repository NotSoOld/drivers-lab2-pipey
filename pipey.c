#include <linux/init.h>    // magic
#include <linux/module.h>  // magic #2
#include <linux/kernel.h>  // magic #3
#include <linux/gfp.h>     // kzalloc flags
#include <linux/slab.h>    // kzalloc
#include <linux/wait.h>    // wait_queue
#include <linux/fs.h>      // file_operations

#define CBUFFERSIZE 65536

int reader_handle;
int writer_handle;
char *cyclic_buffer = NULL;

wait_queue_head_t read_queue;
wait_queue_head_t write_queue;
init_waitqueue_head(&read_queue);
init_waitqueue_head(&write_queue);


//int pipey_open(struct inode *i, struct file *f) {}
//int pipey_release(struct inode *i, struct file *f) {}
//long pipey_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {}

ssize_t pipey_read(struct file *f, char __user *buf, size_t sz, loff_t *off)
{
	// Writes into buf! (user space app READS us)
	unsigned long copied = 0;
	ssize_t written = 0;
	while (true) {
		if (reader_handle == writer_handle) {
			// EOF - no more information coming.
			return 0;
		}
		if (reader_handle < writer_handle) {
			unsigned long available_bytes = writer_handle - reader_handle;
			if (sz < available_bytes) {
				written += copy_to_user(buf, cyclic_buffer + reader_handle, sz);
				reader_handle += sz;
				sz = 0;
			}
			else {
				written += copy_to_user(buf, cyclic_buffer + reader_handle, available_bytes);
				reader_handle += available_bytes;
				sz -= available_bytes;
			}
		}
		else {
			unsigned long available_bytes = CBUFFERSIZE - reader_handle;
			if (sz < available_bytes) {
				written += copy_to_user(buf, cyclic_buffer + reader_handle, sz);
				reader_handle += sz;
				sz = 0;
			}
			else {
				written += copy_to_user(buf, cyclic_buffer + reader_handle, available_bytes);
				reader_handle = 0;
				sz -= available_bytes;
				copied = copy_to_user(buf, cyclic_buffer, sz <= writer_handle ? sz : writer_handle);
				sz -= copied;
				written += copied;
				reader_handle = copied;
			}
		}
		
		if (sz == 0) {
			return written;
		}
		else {
			// Wait for new information to arrive.
			wake_up_interruptible(&read_queue);
			wait_event_interruptible(&write_queue, (reader_handle - writer_handle != 0));
		}
	}
}


ssize_t pipey_write(struct file *f, const char __user *buf, size_t sz, loff_t *off)
{
	// Writes cz bytes to device from buf (user space app WRITES to us)
	unsigned long copied = 0;
	ssize_t readed = 0;
	while (true) {
		if (reader_handle == writer_handle) {
			// Buffer is empty information coming.
			return 0;
		}
		if (writer_handle < reader_handle) {
			unsigned long available_bytes = reader_handle - writer_handle - 1;
			if (sz < available_bytes) {
				readed += copy_from_user(buf, cyclic_buffer + writer_handle, sz);
				writer_handle += sz;
				sz = 0;
			}
			else {
				readed += copy_from_user(buf, cyclic_buffer + writer_handle, available_bytes);
				writer_handle += available_bytes;
				sz -= available_bytes;
			}
		}
		else {
			unsigned long available_bytes = CBUFFERSIZE - writer_handle;
			if (sz < available_bytes) {
				readed += copy_from_user(buf, cyclic_buffer + writer_handle, sz);
				writer_handle += sz;
				sz = 0;
			}
			else {
				readed += copy_from_user(buf, cyclic_buffer + writer_handle, available_bytes);
				writer_handle = 0;
				sz -= available_bytes;
				copied = copy_from_user(buf, cyclic_buffer, sz <= reader_handle ? sz : reader_handle);
				sz -= copied;
				readed += copied;
				writer_handle = copied;
			}
		}
		
		if (sz == 0) {
			return readed;
		}
		else {
			// Wait for new information to depart (to free some buffer space).
			wake_up_interruptible(&write_queue);
			wait_event_interruptible(&read_queue, (writer_handle - reader_handle > 1));
		}
	}
}


char *init_cyclic_buffer(void)
{
	char *cyclic_buffer = (char *)kzalloc(CBUFFERSIZE * sizeof(char), GFP_USER);
	reader_handle = 0;
	writer_handle = 0;
	return cyclic_buffer;
}


static int __init pipey_init(void)
{
	cyclic_buffer = init_cyclic_buffer();
	return 0;
} 


static void __exit pipey_exit(void)
{
	kfree(cyclic_buffer);
}


static const struct file_operations pipey_file_ops = {
	//.open = pipey_open,
	//.release = pipey_release,
	//.unlocked_ioctl = pipey_ioctl,
	.write = pipey_write,
	.read = pipey_read
};

module_init(pipey_init);
module_exit(pipey_exit);
