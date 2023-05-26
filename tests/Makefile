main: main.s
	g++ main.s -std=c++20 -O3 -lfmt -o main

main.s: main.cpp ../src/ringbuffer.h
	g++ main.cpp -S -std=c++20 -O3 -lfmt

clean:
	rm -fr main main.s
	