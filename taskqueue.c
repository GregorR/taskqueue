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

#define _BSD_SOURCE /* for strdup, strsep */
#define _POSIX_SOURCE /* for kill */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "buffer.h"
#include "event.h"
#include "whereami.h"

/* strings */
static char unrecognizedCommand[] = "Unrecognized command.\n";
static char spawnFailed[] = "Failed to spawn your task, the system is probably overloaded. Please wait and try again.\n";
static char taskQueued[] = "Your task has been queued. When it runs, you will receive the result via email.\n";
static char fromAddress[] = "noreply@localhost";
static char fromName[] = "No Reply";


/* tasks are of the form:
 * <begin timeout> <notify timeout> <max queue to block> <email to notify> <subject line> <command> */

/* a single task */
struct Task {
    int socket;
    struct timeval begin;
    struct timeval notify;
    struct event notifyEv;
    int maxBlock;
    char *email, *subject, *cmd;

    pid_t pid;
    int fd;
    struct event ev;
    struct Buffer_char output;
};

/* a pre-task connection */
struct Connection {
    int fd;
    struct event ev;
    struct Buffer_char buf;
};

/* the queue is stored in a buffer */
BUFFER(Task, struct Task *);

/* mailer command to use (htmlsender) */
static char *mailer;

/* max fd (for clean closing) */
static int maxFd;

/* currently active tasks */
static struct Buffer_Task curTasks;

/* maximum number of parallel tasks */
static int parallelTasks;

/* queued tasks */
static struct Buffer_Task taskQueue;

/* the listening UNIX domain socket */
static int cmdSock;

/* the event that's triggered when data is availble from the command FIFO */
static struct event cmdEv;

/* the event that's triggered when a SIGCHLD arrives */
static struct event chldEv;

/* create a new task */
struct Task *newTask(int socket, struct timeval begin, struct timeval notify, int maxBlock, char *email, char *subject, char *cmd)
{
    struct Task *ret;

    SF(ret, malloc, NULL, (sizeof(struct Task)));

    ret->socket = socket;
    ret->begin = begin;
    ret->notify = notify;
    ret->maxBlock = maxBlock;
    SF(ret->email, strdup, NULL, (email));
    SF(ret->subject, strdup, NULL, (subject));
    SF(ret->cmd, strdup, NULL, (cmd));

    /* stuff that isn't used yet */
    ret->pid = -1;
    ret->fd = -1;
    ret->output.buf = NULL;

    return ret;
}

/* destroy a task */
void deleteTask(struct Task *t)
{
    if (t->socket >= 0) close(t->socket);
    free(t->email);
    free(t->subject);
    free(t->cmd);
    if (t->output.buf)
        FREE_BUFFER(t->output);
    free(t);
}

/* remove a task from the task queue (does not free it, just remove it) */
void removeTask(struct Buffer_Task *buf, int index)
{
    memmove(buf->buf + index, buf->buf + index + 1, (buf->bufused - index - 1) * sizeof(struct Task *));
    buf->bufused--;
}

/* make an fd nonblocking */
void nonblocking(int fd)
{
    int flags;
    SF(flags, fcntl, -1, (fd, F_GETFL));
    flags |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}

/* make an fd blocking */
void blocking(int fd)
{
    int flags;
    SF(flags, fcntl, -1, (fd, F_GETFL));
    flags &= ~O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}

/* perform a blocking write to an fd */
void blockingWrite(int fd, char *str)
{
    ssize_t tmpss;
    blocking(fd);
    tmpss = write(fd, str, strlen(str)); (void) tmpss;
    nonblocking(fd);
}

/* close all fds except for the one requested */
void closeFds(int fd)
{
    int i;
    for (i = 0; i < maxFd; i++) {
        if (i != fd) close(i);
    }
}

void cmdConnection(int, short, void *);
void cmdRead(int, short, void *);
void beginTask(int, short, void *);
void enqueueTask(struct Task *);
void runTask(struct Task *);
void killTask(struct Task *);
void taskData(int, short, void *);
void notifyTask(int, short, void *);

/* the event that's fired when we get a connection for a command */
void cmdConnection(int fd, short event, void *ignore)
{
    int tFd;
    struct Connection *conn;

    /* accept the connection */
    tFd = accept(fd, NULL, NULL);
    if (tFd == -1) {
        perror("accept");
        return;
    }
    nonblocking(tFd);

    /* and prepare an in-progress state for it */
    SF(conn, malloc, NULL, (sizeof(struct Connection)));
    conn->fd = tFd;
    INIT_BUFFER(conn->buf);

    /* prepare its event state */
    event_set(&conn->ev, tFd, EV_READ|EV_PERSIST, cmdRead, (void *) conn);
    event_add(&conn->ev, NULL);
}

/* kill a partial connection */
void killConn(struct Connection *conn)
{
    close(conn->fd);
    event_del(&conn->ev);
    FREE_BUFFER(conn->buf);
    free(conn);
}

/* the event that's fired when data is read from a connection that has not yet
 * provided a full command */
void cmdRead(int fd, short event, void *connVp)
{
    struct Connection *conn = connVp;
    char *nl, *part, *saveptr = NULL, *cmd, *email, *subject;
    int maxBlock;
    struct Task *task;
    struct timeval tv, begin, notify;

    /* read in as much as we can */
    while (1) {
        ssize_t rd = read(conn->fd, conn->buf.buf + conn->buf.bufused, conn->buf.bufsz - conn->buf.bufused);
        if (rd == -1) {
            if (errno == EAGAIN) {
                /* fine, no more data */
                break;
            } else {
                /* not good, moan */
                killConn(conn);
                return;
            }

        } else {
            /* actually read something */
            conn->buf.bufused += rd;
            if (conn->buf.bufused >= conn->buf.bufsz) EXPAND_BUFFER(conn->buf);

        }
    }
    conn->buf.buf[conn->buf.bufused] = '\0';

    /* do we have a full line? */
    if (!(nl = strchr(conn->buf.buf, '\n'))) {
        /* nope, nothing left to do */
        return;
    }

    /* we have a full line, interpret it */
    *nl = '\0';
    gettimeofday(&tv, NULL);
    begin = tv;
    notify = tv;

    /* first the command */
    saveptr = conn->buf.buf;
    cmd = strsep(&saveptr, ",");
    if (!cmd) { killConn(conn); return; }

    if (!strcmp(cmd, "enqueue")) {
        /* first, begin time */
        part = strsep(&saveptr, ",");
        if (!saveptr) { killConn(conn); return; }
        begin.tv_sec += atoi(part);

        /* next, notification time */
        part = strsep(&saveptr, ",");
        if (!saveptr) { killConn(conn); return; }
        notify.tv_sec += atoi(part);

        /* then maximum queue to block for */
        part = strsep(&saveptr, ",");
        if (!saveptr) { killConn(conn); return; }
        maxBlock = atoi(part);

        /* then email address */
        email = strsep(&saveptr, ",");
        if (!saveptr) { killConn(conn); return; }

        /* then subject */
        subject = strsep(&saveptr, ",");
        if (!saveptr) { killConn(conn); return; }

        /* finally, the actual command */
        cmd = saveptr;


        /* OK, now create a task structure for it */
        task = newTask(fd, begin, notify, maxBlock, email, subject, cmd);

        /* get rid of the now-useless connection structure */
        event_del(&conn->ev);
        FREE_BUFFER(conn->buf);
        free(conn);

        /* either delay or enqueue this task */
        if (begin.tv_sec > tv.tv_sec) {
            /* delay it */
            task->begin.tv_sec -= tv.tv_sec;
            task->begin.tv_usec = 0;
            evtimer_set(&task->ev, beginTask, (void *) task);
            evtimer_add(&task->ev, &task->begin);
        } else {
            /* enqueue this task */
            enqueueTask(task);
        }

    } else if (!strcmp(cmd, "stat")) {
        char buf[100];
        int ct = taskQueue.bufused + curTasks.bufused;
        snprintf(buf, 100, "%d task%s\n", ct, (ct == 1) ? "" : "s");
        blockingWrite(conn->fd, buf);
        killConn(conn);

    } else if (!strcmp(cmd, "stop")) {
        event_loopbreak();
        killConn(conn);

    } else {
        blockingWrite(conn->fd, unrecognizedCommand);
        killConn(conn);

    }
}

/* begin this task (enqueue it after delay) */
void beginTask(int fd, short event, void *taskVp)
{
    struct Task *task = taskVp;
    enqueueTask(task);
}

/* add this task either immediately to be run, or to a task queue */
void enqueueTask(struct Task *task)
{
    /* check the queue limit */
    if (task->maxBlock >= 0 && taskQueue.bufused >= task->maxBlock) {
        if (task->socket >= 0) {
            blockingWrite(task->socket, taskQueued);
            close(task->socket);
        }
        task->socket = -1;
    }

    if (curTasks.bufused < parallelTasks) {
        /* just run it */
        runTask(task);
        return;
    }

    /* add it to the queue */
    WRITE_ONE_BUFFER(taskQueue, task);
}

/* actually run this task */
void runTask(struct Task *task)
{
    int taskIo[2], tmpi;
    pid_t pid;


    /* add it to the current tasks */
    WRITE_ONE_BUFFER(curTasks, task);

    /* prepare a pipe for its output */
    tmpi = pipe(taskIo);
    if (tmpi == -1) {
        /* massive failure, kill it */
        if (task->socket >= 0) blockingWrite(task->socket, spawnFailed);
        killTask(task);
        return;
    }

    /* and fork */
    pid = fork();
    if (pid == -1) {
        if (task->socket >= 0) blockingWrite(task->socket, spawnFailed);
        close(taskIo[0]);
        close(taskIo[1]);
        killTask(task);
        return;
    }

    if (pid == 0) {
        /* child, only need the write end of the pipe */
        closeFds(taskIo[1]);

        /* set up std{in,out,err} */
        dup2(open("/dev/null", O_RDONLY), 0);
        dup2(taskIo[1], 1);
        dup2(taskIo[1], 2);
        close(taskIo[1]);

        /* then run the task */
        exit(system(task->cmd));
        exit(-1);
        abort();
    }

    /* OK, we're the parent, so just wait for data or SIGCHLD */
    close(taskIo[1]);
    task->pid = pid;
    task->fd = taskIo[0];
    nonblocking(task->fd);
    INIT_BUFFER(task->output);

    event_set(&task->ev, task->fd, EV_READ|EV_PERSIST, taskData, (void *) task);
    event_add(&task->ev, NULL);
}

/* kill this running task brutally */
void killTask(struct Task *task)
{
    int i, idx = -1;

    /* find its index */
    for (i = 0; i < curTasks.bufused; i++) {
        if (curTasks.buf[i] == task) {
            idx = i;
            break;
        }
    }

    /* remove it from the list */
    if (idx != -1) {
        removeTask(&curTasks, idx);
    }

    /* kill the process */
    if (task->pid > 0) {
        event_del(&task->ev);
        kill(task->pid, SIGKILL);
        close(task->fd);
    }

    /* free the memory */
    deleteTask(task);
}

/* this task has received data */
void taskData(int fd, short event, void *taskVp)
{
    struct Task *task = taskVp;

    /* read it all in */
    while (1) {
        ssize_t rd = read(fd, task->output.buf + task->output.bufused, task->output.bufsz - task->output.bufused);
        if (rd <= 0) {
            if (rd == 0 || errno == EAGAIN) {
                /* fine, no more data */
                break;
            } else {
                /* not good, moan */
                killTask(task);
                return;
            }

        } else {
            /* actually read something */
            task->output.bufused += rd;
            if (task->output.bufused >= task->output.bufsz) EXPAND_BUFFER(task->output);

        }
    }
    task->output.buf[task->output.bufused] = '\0';
}

/* we've received a SIGCHLD */
void sigChild(int a, short b, void *c)
{
    /* first figure out from whom */
    pid_t pid;
    int i;
    struct timeval tv, tv2;
    
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        /* figure out the task */
        struct Task *task = NULL;

        for (i = 0; i < curTasks.bufused; i++) {
            if (curTasks.buf[i]->pid == pid) {
                task = curTasks.buf[i];
                break;
            }
        }

        /* if we didn't find it, not much we can do */
        if (task == NULL) continue;

        /* remove the task from the list */
        removeTask(&curTasks, i);

        /* free its fd and read event */
        close(task->fd);
        event_del(&task->ev);

        /* and run a task if there are any in the queue */
        if (taskQueue.bufused > 0) {
            struct Task *nextTask = taskQueue.buf[0];
            removeTask(&taskQueue, 0);
            runTask(nextTask);
        }

        /* figure out when to notify the task */
        gettimeofday(&tv, NULL);
        if (tv.tv_sec >= task->notify.tv_sec) {
            /* notify it now! */
            notifyTask(0, 0, (void *) task);
        } else {
            /* time math */
            tv2 = task->notify;
            if (tv2.tv_usec < tv.tv_usec) {
                tv2.tv_usec += 1000000;
                tv2.tv_sec--;
            }
            tv2.tv_sec -= tv.tv_sec;
            tv2.tv_usec -= tv.tv_usec;

            /* now queue it */
            evtimer_set(&task->notifyEv, notifyTask, (void *) task);
            evtimer_add(&task->notifyEv, &tv2);
        }
    }
}

/* notify a task (via socket if applicable, and email) of its output */
void notifyTask(int fd, short event, void *taskVp)
{
    struct Task *task = taskVp;
    char tempFile[] = "/tmp/notify.XXXXXX";
    int tempFd;
    pid_t pid;
    ssize_t wr;

    /* notify via socket */
    if (task->socket >= 0) {
        blockingWrite(task->socket, task->output.buf);
        close(task->socket);
        task->socket = -1;
    }

    /* then notify via email */
    if (task->email[0] && (tempFd = mkstemp(tempFile)) >= 0) {
        /* write in the data */
        wr = write(tempFd, "<html><head><title>", 19);
        wr = write(tempFd, task->subject, strlen(task->subject));
        wr = write(tempFd, "</title></head><body>", 21);
        wr = write(tempFd, task->output.buf, task->output.bufused);
        wr = write(tempFd, "</body></html>", 14);
        (void) wr;
        close(tempFd);

        /* then call our mailer */
        pid = fork();
        if (pid == 0) {
            /* don't tell the mailer anything */
            closeFds(-1);
            dup2(open("/dev/null", O_RDONLY), 0);
            dup2(open("/dev/null", O_WRONLY), 1);
            dup2(1, 2);

            /* run it */
            execl(mailer, mailer, "-t", task->email, "-f", fromAddress, "-F", fromName, "-s", task->subject, "-b", tempFile, NULL);
            exit(-1);
            abort();

        } else if (pid > 0) {
            /* reap it and delete the temp file */
            waitpid(pid, NULL, 0);
            unlink(tempFile);

        }
    }

    deleteTask(task);
}

#define ARG(s, l) if (!strcmp(arg, "-" #s) || !strcmp(arg, "--" #l))
#define ARGN(s, l) if ((!strcmp(arg, "-" #s) || !strcmp(arg, "--" #l)) && argn)
void usage(void);

/* main loop */
int main(int argc, char **argv)
{
    int i, tmpi;
    char *cmdSockFn = NULL;
    struct sockaddr_un sun;
    struct rlimit rl;
    char *dir, *fil;

    /* initialization */
    INIT_BUFFER(curTasks);
    INIT_BUFFER(taskQueue);
    SF(tmpi, getrlimit, -1, (RLIMIT_NOFILE, &rl));
    maxFd = rl.rlim_cur;
    parallelTasks = 1;

    /* use whereAmI to figure out our mailer */
    if (!whereAmI(argv[0], &dir, &fil)) {
        fprintf(stderr, "Cannot figure out my path, assuming /usr/bin.");
        dir = "/usr/bin";
    }

    SF(mailer, malloc, NULL, (strlen(dir) + 12));
    sprintf(mailer, "%s/htmlsender", dir);

    /* handle arguments */
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        char *argn = argv[i+1];


        ARGN(s, socket) {
            cmdSockFn = argn;
            i++;

        } else ARGN(j, parallel-tasks) {
            parallelTasks = atoi(argn);
            i++;

        } else {
            usage();
            return 1;

        }
    }

    if (cmdSockFn == NULL) {
        usage();
        return 1;
    }

    /* create and open the socket */
    unlink(cmdSockFn); /* ignore result */
    SF(cmdSock, socket, -1, (AF_UNIX, SOCK_STREAM, 0));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, cmdSockFn, sizeof(sun.sun_path));
    SF(tmpi, bind, -1, (cmdSock, (struct sockaddr *) &sun, sizeof(sun)));
    SF(tmpi, listen, -1, (cmdSock, 32));
    nonblocking(cmdSock);

    /* prepare our event loop */
    event_init();
    event_set(&cmdEv, cmdSock, EV_READ|EV_PERSIST, cmdConnection, NULL);
    event_add(&cmdEv, NULL);
    signal_set(&chldEv, SIGCHLD, sigChild, NULL);
    signal_add(&chldEv, NULL);

    /* and go */
    event_loop(0);

    return 0;
}

/* usage statement */
void usage()
{
    fprintf(stderr, "Use: taskqueue -s <socket> [options]\n"
            "Options:\n"
            "\t--socket|-s <socket file>: Unix domain socket to read commands from.\n"
            "\n");
}
