CC:=/usr/bin/x86_64-w64-mingw32-gcc
CXX:=/usr/bin/x86_64-w64-mingw32-g++
STRIP:=/usr/bin/x86_64-w64-mingw32-strip
CFLAGS:=-I/dev/shm/win32/include -Wall -ggdb3 -std=c99 -Wall -Wno-comment -Wno-unused-variable -Wno-narrowing -I/dev/shm/win32/include
CXXFLAGS:=$(CFLAGS) -fpermissive
LIBS=-lwinmm -lws2_32 -lpthread -lmp3lame
LDFLAGS=-L/dev/shm/win32/lib -static -static-libgcc -static-libstdc++ -lpthread
OUTNAME=streamer.exe
C_OBJECTS:=main.o compress.o

all: $(OUTNAME)

release: $(OUTNAME)
	$(STRIP) --strip-all $^
	upx -v9 $^
	7z a $^.7z $^

$(OUTNAME): $(C_OBJECTS)
	$(CC) $(C_OBJECTS) $(CFLAGS) $(LDFLAGS) $(LIBS) -o $(OUTNAME)

clean: $(C_OBJECTS) $(CXX_OBJECTS)
	rm -vf $^ $(OUTNAME)
