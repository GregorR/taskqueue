CC=gcc
CFLAGS=-g -O2
LDFLAGS=
LIBS=-levent

TASKQUEUE_OBJS=taskqueue.o whereami.o
TASKENQUEUE_OBJS=taskenqueue.o
TASKQUEUESTAT_OBJS=taskqueuestat.o

all: taskqueue taskenqueue taskqueuestat

taskqueue: $(TASKQUEUE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TASKQUEUE_OBJS) $(LIBS) -o taskqueue

taskenqueue: $(TASKENQUEUE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TASKENQUEUE_OBJS) -o taskenqueue

taskqueuestat: $(TASKQUEUESTAT_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TASKQUEUESTAT_OBJS) -o taskqueuestat

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TASKQUEUE_OBJS) taskqueue
	rm -f $(TASKENQUEUE_OBJS) taskenqueue
	rm -f $(TASKQUEUESTAT_OBJS) taskqueuestat
