#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define DRIVER_PATH "/dev/deep"
#define SET_SIZE_OF_QUEUE _IOW('a', 'a', int *)

int main(void) {
    int fd;
    int size = 100;

    fd = open(DRIVER_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open the device");
        return -1;
    }

    printf("sending size %d\n", size);
    if (ioctl(fd, SET_SIZE_OF_QUEUE, &size) < 0) {
        perror("ioctlfailed");
        close(fd);
        return -1;
    }

    printf("config done....\n");
    close(fd);
    return 0;
}
