#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>

MODULE_LICENSE("GPL");


#define DRIVER_NAME "deep"
#define CLASS_NAME "vicharak"


#define SET_SIZE_OF_QUEUE _IOW('a', 'a', int *)
#define PUSH_DATA _IOW('a', 'b', struct user_data)
#define POP_DATA _IOWR('a', 'c', struct user_data)

struct user_data
{
    int length;
    char __user *data;
};
struct queue_item
{
    char *data;
    int length;
};
struct cq
{
    struct queue_item *items;
    int capacity, size, head, tail;
    struct mutex lock;
    wait_queue_head_t read_q, write_q;
};

static int __init chardev_init(void);
static void __exit chardev_exit(void);
static long my_ioctl(struct file *file, unsigned int cmd, unsigned long arg);


static dev_t dev_num;
static struct cdev my_cdev;
static struct class *my_class;
static struct cq *gq; 

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = my_ioctl,
};

static int __init chardev_init(void)
{
    printk(KERN_INFO "init ..");
    gq = NULL;
    if (alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME) < 0)
        return -1;
    my_class = class_create(CLASS_NAME);
    if (IS_ERR(my_class))
    {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(my_class);
    }
    if (IS_ERR(device_create(my_class, NULL, dev_num, NULL, DRIVER_NAME)))
    {
        class_destroy(my_class);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }
    cdev_init(&my_cdev, &fops);
    if (cdev_add(&my_cdev, dev_num, 1) < 0)
    {
        device_destroy(my_class, dev_num);
        class_destroy(my_class);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }
    return 0;
}

static void __exit chardev_exit(void)
{
    if (gq)
    {
        printk(KERN_INFO "freeing q.\n");
        if (gq->items)
        {
            for (int i = 0; i < gq->size; i++)
            {
                kfree(gq->items[(gq->tail + i) % gq->capacity].data);
            }
            kfree(gq->items);
        }
        kfree(gq);
    }
    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);
    printk(KERN_INFO " Module unloaded.\n");
}

static long my_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct user_data u_data;
    int dsize;
    long ret = 0;

   
    switch (cmd)
    {
    case SET_SIZE_OF_QUEUE:
        if (gq)
        {
            printk(KERN_WARNING " already configured.\n");
            return -EBUSY;
        }
        if (copy_from_user(&dsize, (int *)arg, sizeof(int)))
            return -EFAULT;
        if (dsize <= 0)
            return -EINVAL;
        gq = kzalloc(sizeof(struct cqueue), GFP_KERNEL);
        if (!gq)
            return -ENOMEM;
        gq->items = kzalloc(sizeof(struct queue_item) * dsize, GFP_KERNEL);
        if (!gq->items)
        {
            kfree(gq);
            gq = NULL;
            return -ENOMEM;
        }
        gq->capacity = dsize;
        mutex_init(&gq->lock);
        init_waitqueue_head(&gq->read_q);
        init_waitqueue_head(&gq->write_q);
        printk(KERN_INFO "configured with capacity %d\n", gq->capacity);
        break;

    case PUSH_DATA:
        if (copy_from_user(&u_data, (struct user_data *)arg, sizeof(struct user_data)))
            return -EFAULT;
        if (u_data.length <= 0)
            return -EINVAL;

        if (mutex_lock_interruptible(&gq->lock))
            return -ERESTARTSYS;
        while (gq->size == gq->capacity)
        {
            mutex_unlock(&gq->lock);
            if (wait_event_interruptible(gq->write_q, gq->size < gq->capacity))
                return -ERESTARTSYS;
            if (mutex_lock_interruptible(&gq->lock))
                return -ERESTARTSYS;
        }

        gq->items[gq->head].length = u_data.length;
        gq->items[gq->head].data = kmalloc(u_data.length, GFP_KERNEL);
        if (!gq->items[gq->head].data)
        {
            ret = -ENOMEM;
            goto push_unlock;
        }
        if (copy_from_user(gq->items[gq->head].data, u_data.data, u_data.length))
        {
            kfree(gq->items[gq->head].data);
            ret = -EFAULT;
            goto push_unlock;
        }

        gq->head = (gq->head + 1) % gq->capacity;
        gq->size++;
        printk(KERN_INFO "Pushed %d bytes , size: %d\n", u_data.length, gq->size);

    push_unlock:
        mutex_unlock(&gq->lock);
        wake_up_interruptible(&gq->read_q);
        break;

    case POP_DATA:
        if (mutex_lock_interruptible(&gq->lock))
            return -ERESTARTSYS;
        while (gq->size == 0)
        {
            mutex_unlock(&gq->lock);
            if (wait_event_interruptible(gq->read_q, gq->size > 0))
                return -ERESTARTSYS;
            if (mutex_lock_interruptible(&gq->lock))
                return -ERESTARTSYS;
        }

        if (copy_from_user(&u_data, (struct user_data *)arg, sizeof(struct user_data)))
        {
            ret = -EFAULT;
            goto pop_unlock;
        }
        if (u_data.length < gq->items[gq->tail].length)
        {
            ret = -EINVAL;
            goto pop_unlock;
        }
        if (copy_to_user(u_data.data, gq->items[gq->tail].data, gq->items[gq->tail].length))
        {
            ret = -EFAULT;
            goto pop_unlock;
        }
        if (copy_to_user(&((struct user_data *)arg)->length, &gq->items[gq->tail].length, sizeof(int)))
        {
            ret = -EFAULT;
            goto pop_unlock;
        }

        kfree(gq->items[gq->tail].data);
        gq->tail = (gq->tail + 1) % gq->capacity;
        gq->size--;
        printk(KERN_INFO "Popped data ,  size: %d\n", gq->size);

    pop_unlock:
        mutex_unlock(&gq->lock);
        wake_up_interruptible(&gq->write_q);
        break;

    default:
        return -ENOTTY;
    }
    return ret;
}
module_init(chardev_init);
module_exit(chardev_exit);
