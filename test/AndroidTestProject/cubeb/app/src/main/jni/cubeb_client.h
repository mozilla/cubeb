//
// Created by Alex Chronopoulos on 5/20/16.
//

#ifndef CUBEB_CUBEB_CLIENT_H
#define CUBEB_CUBEB_CLIENT_H

enum mode {
    READ,
    WRITE,
    READ_WRITE,
};

int
init_client();

int
init_stream(enum mode m);

int
start_stream();

int
stop_stream();

int
destroy_stream();

int
destroy_client();

#endif //CUBEB_CUBEB_CLIENT_H
