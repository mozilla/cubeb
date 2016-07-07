//
// Created by Alex Chronopoulos on 5/27/16.
//

#ifndef CUBEB_HELLO_JNI_UTILS_H
#define CUBEB_HELLO_JNI_UTILS_H

#include <sys/types.h>

int append_on_file(const char * file_path, const void * buf, size_t size_buf);
int remove_file(const char * file_path);

#endif //CUBEB_HELLO_JNI_UTILS_H
