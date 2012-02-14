/*
 * Copyright (C) 2012 Gregor Richards
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "helpers.h"

#define BUFSZ 1024

int main(int argc, char **argv)
{
    int sock, i, tmpi;
    struct sockaddr_un sun;
    char buf[BUFSZ];
    ssize_t rd, wr;

    if (argc < 8) {
        fprintf(stderr, "Use: taskenqueue <socket> <begin> <notify> <max block> <email> <subject> <cmd>\n");
        return 1;
    }

    /* connect the socket */
    SF(sock, socket, -1, (AF_UNIX, SOCK_STREAM, 0));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, argv[1], sizeof(sun.sun_path));
    SF(tmpi, connect, -1, (sock, (struct sockaddr *) &sun, sizeof(sun)));

    /* write the command */
    wr = write(sock, "enqueue", 7);
    for (i = 2; i < argc; i++) {
        wr = write(sock, (i < 8) ? "," : " ", 1);
        wr = write(sock, argv[i], strlen(argv[i]));
    }
    wr = write(sock, "\n", 1);

    /* now just wait for its response */
    while ((rd = read(sock, buf, BUFSZ)) > 0)
        wr = write(1, buf, rd);
    close(sock);

    (void) wr;

    return 0;
}
