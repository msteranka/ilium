#define ILIUM_DEBUG /* For printk debugging */
#undef PDEBUG
#ifdef ILIUM_DEBUG
#   define PDEBUG(fmt, args...) printk(KERN_DEBUG "debug: " fmt, ## args)
#else
#   define PDEBUG(fmt, args...) 
#endif

#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/errno.h>

struct ilium_dev {
    void *data;                 /* Array list */
    struct cdev cdev;           /* Char device registration */
    struct semaphore sem;       /* Mutual exclusion semaphore */
    long size, capacity;        /* Current size and maximum capacity */
    unsigned char pages_pow;    /* log2(num_pages) */
};

int ilium_open(struct inode *, struct file *);
int ilium_release(struct inode *, struct file *);
ssize_t ilium_write(struct file *, const char __user *, size_t, loff_t *);
ssize_t ilium_read(struct file *, char __user *, size_t, loff_t *);
loff_t ilium_llseek(struct file *, loff_t, int);

dev_t devno; /* Major and minor device numbers */
struct ilium_dev ilium_dev;
struct file_operations ilium_fops = { /* File operations for ilium_dev */
    .owner = THIS_MODULE,
    .open = ilium_open,
    .release = ilium_release,
    .write = ilium_write,
    .read = ilium_read,
    .llseek = ilium_llseek,
};

int ilium_open(struct inode *inode, struct file *filp)
{
    struct ilium_dev *devp = container_of(inode->i_cdev, struct ilium_dev, cdev); /* Fetch device pointer */
    filp->private_data = (void *) devp; /* Assign to private data for future write and read calls */
    PDEBUG("Ilium: successfully opened\n");
    return 0;
}

int ilium_release(struct inode *inode, struct file *filp)
{
    PDEBUG("Ilium: successfully released\n");
    return 0;
}

ssize_t ilium_write(struct file *filp, const char __user *buf, size_t count, loff_t *offp)
{
    struct ilium_dev *dev = (struct ilium_dev *) filp->private_data; /* Fetch device pointer */
    int retval;

    if (down_interruptible(&dev->sem) != 0) { /* Critical section */
        return -ERESTARTSYS;
    }

    if (*offp > dev->size) { /* Check that offset doesn't exceed device size */
        retval = -EFAULT;
        goto fail;
    }

    while (*offp + count > dev->capacity) { /* If writing data will exceed device capacity, then double the array list size */
        void *ptr = (void *) __get_free_pages(GFP_KERNEL, dev->capacity << 1);
        if (ptr == NULL) { /* If more memory could not be allocated */
            count = dev->capacity - *offp; /* Then write as much as possible */
            break;
        }
        memcpy(ptr, dev->data, dev->size); /* Copy old data */
        memset(ptr + dev->size, 0, (dev->capacity << 1) - dev->size); /* Zero out the rest to prevent data leaks */
        free_pages((unsigned long) dev->data, dev->pages_pow);
        dev->data = ptr;
        dev->capacity <<= 1;
    }

    copy_from_user(dev->data + *offp, buf, count);
    dev->size += count;
    *offp += count;
    retval = count;
    PDEBUG("Ilium: successfully wrote %lu bytes of %s\n", count, buf);

    fail:
        up(&dev->sem); /* Critical section end */
        return retval;
}

ssize_t ilium_read(struct file *filp, char __user *buf, size_t count, loff_t *offp)
{
    struct ilium_dev *dev = (struct ilium_dev *) filp->private_data;
    int retval;

    if (down_interruptible(&dev->sem) != 0) {
        return -ERESTARTSYS;
    }

    if (*offp >= dev->size) { /* If offset exceeds device size */
        retval = -EFAULT;
        PDEBUG("Ilium: failed because *offp > dev->size\n");
        goto fail;
    }

    if (*offp + count > dev->size) { /* If read will exceed data size, then read as much as possible */
        count = dev->size - *offp;
    }

    copy_to_user(buf, dev->data + *offp, count);
    retval = count;
    *offp += count;
    PDEBUG("Ilium: successfully read %lu bytes of %s\n", count, buf);

    fail:
        up(&dev->sem);
        return retval;
}

loff_t ilium_llseek(struct file *filp, loff_t off, int whence)
{
    struct ilium_dev *devp = (struct ilium_dev *) filp->private_data;
    loff_t retval = -EFAULT;

    if (down_interruptible(&devp->sem) != 0) {
        return retval;
    }

    switch (whence) { /* Check value of whence for appropiate action */
        case SEEK_SET:
            if (off >= devp->size) { /* If desired offset exceeds device size */
                goto fail;
            }
            filp->f_pos = off;
            break;
        case SEEK_CUR:
            if (filp->f_pos + off >= devp->size) { /* If incrementing the current offset by desired amount exceeds device size */
                goto fail;
            }
            filp->f_pos += off;
            break;
        case SEEK_END:
            if (devp->size - 1 + off >= devp->size) { /* If adding off to the end of the file exceeds device size */ 
                goto fail;
            }
            filp->f_pos = devp->size - 1 + off;
            break;
    }
    retval = filp->f_pos;

    fail:
        up(&devp->sem);
        return retval;
}

static int __init ilium_init(void)
{
    int err;

    ilium_dev.pages_pow = 5; /* List initially has 2^5 pages */
    ilium_dev.size = 0;
    ilium_dev.capacity = PAGE_SIZE << ilium_dev.pages_pow;
    ilium_dev.data = (void *) __get_free_pages(GFP_KERNEL, ilium_dev.pages_pow); /* Fetch 32 pages */
    if (ilium_dev.data == NULL) {
        err = -ENOMEM;
        goto get_mem_fail;   
    }
    memset(ilium_dev.data, 0, ilium_dev.capacity); /* Zero out to prevent any information leaks */
    sema_init(&ilium_dev.sem, 1);

    err = alloc_chrdev_region(&devno, 0, 1, "ilium"); /* Fetch major device number */
    if (err < 0) goto register_chrdev_fail;

    cdev_init(&ilium_dev.cdev, &ilium_fops); /* Initialize and register character device */
    ilium_dev.cdev.owner = THIS_MODULE;
    ilium_dev.cdev.ops = &ilium_fops;
    cdev_add(&ilium_dev.cdev, devno, 1);
    if (err < 0) goto cdev_add_fail;

    PDEBUG("Ilium has been successfully registered\n");
    return 0;

    cdev_add_fail: /* Clean up resources if error occurs */
        unregister_chrdev_region(devno, 1);
    register_chrdev_fail:
        free_pages((unsigned long) ilium_dev.data, 3);
    get_mem_fail:
        printk(KERN_NOTICE "Ilium: error %d", err);
    return err;
}

static void __exit ilium_exit(void)
{
    cdev_del(&ilium_dev.cdev);
    unregister_chrdev_region(devno, 1);
    free_pages((unsigned long) ilium_dev.data, ilium_dev.pages_pow);
    PDEBUG("Ilium has been successfully unregistered\n");
}

module_init(ilium_init);
module_exit(ilium_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michael Steranka");
