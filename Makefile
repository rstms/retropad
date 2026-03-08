CC=gcc
WINDRES = windres
CFLAGS = -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -mwindows
LDFLAGS = -mwindows
LIBS = -luser32 -lgdi32 -lcomdlg32 -lcomctl32 -lshell32

OBJS = retropad.o file_io.o retropad.res.o

all: retropad.exe

retropad.exe: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

retropad.o: retropad.c resource.h file_io.h
	$(CC) $(CFLAGS) -c retropad.c

file_io.o: file_io.c file_io.h resource.h
	$(CC) $(CFLAGS) -c file_io.c

retropad.res.o: retropad.rc resource.h res/retropad.ico
	$(WINDRES) $< $@

clean:
	rm -f retropad.exe $(OBJS)
