.PHONY: all
all: nyuenc

nyuenc: nyuenc.o
	gcc -pthread -o nyuenc nyuenc.o

nyuenc.o: nyuenc.c 
	gcc -pthread -c -O3 nyuenc.c

.PHONY: clean
clean:
	rm -f *.o nyuenc
