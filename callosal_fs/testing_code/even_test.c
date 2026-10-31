#include<stdio.h>
#include<unistd.h>
#include <fcntl.h>
//#include <sys/type.h>
#include <sys/stat.h>
#include <stdlib.h>
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
	for(int i = 0; i < 10; i++){
		while(1){
			fstat(fd, stat_buf);
			if((stat_buf->st_size) % 2 == 0){
				ssize_t amount_written = write(fd, testbuf, 1);
				break;
			}
		}
	}
	close(fd);
	printf("done with even");
}
