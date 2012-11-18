DEBUG=0
CC=gcc
NAME = vfs_archive
ifeq ($(DEBUG),1)
CFLAGS = -g -c -std=c99 -fPIC -Wall -DDEBUG
else
CFLAGS = -c -std=c99 -fPIC -Wall
endif
LDFLAGS = -shared -fPIC -larchive
DST_OBJS = $(NAME).o

$(NAME).so: $(DST_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

install: $(NAME).so
	-mkdir -p ~/.local/lib/deadbeef
	-cp $(NAME).so ~/.local/lib/deadbeef

uninstall:
	-rm -rf ~/.local/lib/deadbeef/$(NAME).so

clean:
	-rm -rf *.so *.o

