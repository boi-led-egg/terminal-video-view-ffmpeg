all:
	g++ -std=c++11 video-viewer.cpp -Wall -Wextra -pedantic -o terminal-video-viewer -lavcodec -lavformat  -lswresample -lswscale -lavutil -O2

clean:
	rm terminal-video-viewer
