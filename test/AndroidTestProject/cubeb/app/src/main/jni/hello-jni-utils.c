//
// Created by Alex Chronopoulos on 5/27/16.
//

#include "hello-jni-utils.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>

/* A simple routine calling UNIX write() in a loop */
static ssize_t
loop_write(int fd, const void * data, size_t size) {
    ssize_t ret = 0;

    while (size > 0) {
        ssize_t r;

        if ((r = write(fd, data, size)) < 0) {
            return r;
        }

        if (r == 0) {
            break;
        }

        ret += r;
        data = (const uint8_t*) data + r;
        size -= (size_t) r;
    }

    return ret;
}

int append_on_file(const char * file_path, const void * buf, size_t size_buf)
{
    int fd = open(file_path, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH );
    int sz = loop_write(fd, buf, size_buf);
    close(fd);
    return sz;
}

int remove_file(const char * file_path)
{
    return remove(file_path);
}

