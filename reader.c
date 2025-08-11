#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define DRIVER_PATH "/dev/deep"
#define POP_DATA _IOWR('a', 'c', struct user_data )

struct user_data {
    int length;
    char *data;
};

int main(void) {
    int fd = open(DRIVER_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open driver");
        return -1;
    }

    struct user_data d;
    d.length = 100; 
    d.data = malloc(d.length);

    printf("popping .....\n");
    if (ioctl(fd, POP_DATA, &d) < 0) {
        perror("ioctl failed");
    } else {
        printf("popped %d bytes: '%.*s'\n", d.length, d.length, d.data);
    }

    close(fd);
    free(d.data);
    printf("done.\n");
    return 0;
}

