CC=gcc
CFLAGS=-g -O2
LDFLAGS=
LIBS=-levent

TASKQUEUE_OBJS=taskqueue.o
TASKENQUEUE_OBJS=taskenqueue.o

all: taskqueue taskenqueue

taskqueue: $(TASKQUEUE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TASKQUEUE_OBJS) $(LIBS) -o taskqueue

taskenqueue: $(TASKENQUEUE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TASKENQUEUE_OBJS) -o taskenqueue

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TASKQUEUE_OBJS) taskqueue
	rm -f $(TASKENQUEUE_OBJS) taskenqueue
