#include<stdio.h>
#include<unistd.h>
#include <fcntl.h>
//#include <sys/type.h>
#include <sys/stat.h>
#include <stdlib.h>

//uses sleep 
//https://www.geeksforgeeks.org/sleep-function-in-c/
#include <errno.h>
int main(){   
	char * testbuf = malloc(sizeof(char) * 4);
	testbuf[0] = '0';
	testbuf[1] = 'a';
	testbuf[2] = 'a';
	testbuf[3] = 'a';  //open with append mode
	int fd = open("testfile", O_APPEND | O_WRONLY, 0);
	if(fd){
		printf("file descriptor was %d\n", fd);
	}else{
		close(fd);
		return 1;
	}   
	struct stat * stat_buf = malloc(sizeof(struct stat));
	for(int i = 0; i < 1000; i++){
		int test_rand = rand();
		printf("%d\n", i);
		ssize_t amount_written = write(fd, testbuf, 1);
		usleep(10000 * (test_rand % 10)); 
	}
	close(fd);
	printf("done spamming appends");
}
