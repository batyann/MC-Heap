all:
	gcc -m32 -Wall -g -o heap-test mc_heap_test.c
	gcc -m32 -O3 -Wall -DMAX_PERF -o heap-test-fast mc_heap_test.c

clean:
	rm -f heap-test heap-test-fast
