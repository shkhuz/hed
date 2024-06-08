all:
	g++ -g -O0 -Wall -Wextra main.cpp
	./a.out

debug:
	g++ -D_DEBUG -g -O0 -Wall -Wextra main.cpp
	gdb ./a.out

