obj-m += RL_test.o
RL_test-objs := range_lock_test.o range_lock.o
all:
	 make -C ../. M=$(PWD) modules
clean:
	 make -C ../. M=$(PWD) clean
