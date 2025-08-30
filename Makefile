CC = gcc
CFLAGS = -Wall -O2 $(shell pkg-config --cflags libavformat libavcodec libavutil libswscale libswresample sdl2 ncurses)
LDFLAGS = $(shell pkg-config --libs libavformat libavcodec libavutil libswscale libswresample sdl2 ncurses)

TARGET = bewatermyfriend
SRC = bewatermyfriend.c 

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

