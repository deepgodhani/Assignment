#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define DRIVER_PATH "/dev/deep"
#define PUSH_DATA _IOW('a', 'b', struct user_data )

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
    d.length = 3;
    d.data = malloc(d.length);
    memcpy(d.data, "xyz", d.length);

    printf("pushing '%s' (%d bytes) to the q.\n", d.data, d.length);
    if (ioctl(fd, PUSH_DATA, &d) < 0) {
        perror("ioctl failed");
    }

    close(fd);
    free(d.data);
    printf("done.\n");
    return 0;
}
