# nn_osal POSIX-backend smoke test — run on any Linux host:  make && make run
CC      ?= cc
CFLAGS  += -Wall -Wextra -Werror -O1 -g -DCONFIG_NN_OSAL_BACKEND_POSIX=1 \
           -I../../include -pthread
SRCS     = ../../src/posix/thread.c ../../src/posix/sync.c ../../src/posix/time.c \
           ../../src/posix/log.c ../../src/posix/system.c ../../src/posix/socket.c \
           ../../src/posix/storage.c ../../src/posix/gpio.c smoke.c
smoke: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)
kv_dispatch: ../../src/posix/storage.c kv_dispatch.c
	$(CC) $(CFLAGS) -o $@ ../../src/posix/storage.c kv_dispatch.c
run: smoke kv_dispatch
	./smoke
	./kv_dispatch
clean:
	rm -f smoke kv_dispatch
