DEBUG = -O1
DEF = $(DEBUG) -D_REENTRANT_ -march=pentium -DCFIFO
CPP = g++
LD = g++
INC = -I.

all: test
test : test.o coroutine.o
	$(LD) -o test test.o coroutine.o -pthread
FORCE:	
clean : FORCE
	rm -f *.o test

.cpp.o:	
	$(CPP) -c $(DEF) $(INC) $< -o $@
