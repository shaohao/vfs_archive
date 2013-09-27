APP = vfs_archive
BIN = $(APP).so
DEBUG ?= 0

CC = gcc

CFLAGS = -c -std=c99 -fPIC -Wall
ifeq ($(DEBUG),1)
CFLAGS += -g -DDEBUG
endif

LDFLAGS = -shared -fPIC -larchive

OBJS = $(APP).o

.PHONY: $(BIN)
$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

install: $(APP).so
	-mkdir -p ~/.local/lib/deadbeef
	-cp $(APP).so ~/.local/lib/deadbeef

uninstall:
	-rm -rf ~/.local/lib/deadbeef/$(APP).so

clean:
	-rm -rf *.so *.o

