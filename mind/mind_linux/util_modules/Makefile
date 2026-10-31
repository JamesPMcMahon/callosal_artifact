obj-m += pprint.o
pprint-objs := ./profile_printer/profile_printer.o
obj-m += pclean.o
pclean-objs := ./profile_printer/profile_cleaner.o
obj-m += buggen.o
buggen-objs := ./kern_bug_generator/kern_bug_generator.o
all:
	 make -C ../. M=$(PWD) modules
clean:
	 make -C ../. M=$(PWD) clean
