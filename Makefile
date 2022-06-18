CC = g++

all:
	${CC} -std=c++11 video-viewer.cpp -Wall -Wextra -pedantic -o terminal-video-viewer -lavcodec -lavformat -lswscale -lavutil -O2

clean:
	rm terminal-video-viewer
