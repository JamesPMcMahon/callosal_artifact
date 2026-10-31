#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/file.h>
#include <stdint.h>

//thread_id is the writer thread id

//copied from fxmark

uint64_t usec(void){
	struct timeval tv;
	gettimeofday(&tv, 0);
	return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

int writer_thread_id = -1;
int reader_thread_id = -1;

//command line argument review: https://www.geeksforgeeks.org/command-line-arguments-in-c-cpp/
//string to int: https://www.geeksforgeeks.org/convert-string-to-int-in-c/
int main(int argc, char * argv[]){


	//*********************************
	//start file setup
	//*********************************

	writer_thread_id = 0;
	reader_thread_id = 0;
	int buffer_file;
	int metadata_file;
	char buffer_name[11] = "x_x_buffer\0";
	char  metadata_name[13] = "x_x_metadata\0";

	//set up the buffer name
	buffer_name[0] = 48 + writer_thread_id;
	buffer_name[2] = 48 + reader_thread_id;
	//set up the metadata name
	metadata_name[0] = 48 + writer_thread_id;
	metadata_name[2] = 48 + reader_thread_id;

	buffer_file = open(buffer_name, O_RDWR, 0666);
	if(buffer_file == -1){
		printf("failed to create file");
		return -1;
	}
	metadata_file = open(metadata_name, O_RDWR, 0666);
	if(metadata_file == -1){
		printf("failed to create file");
		return -1;
	}

	//*********************************
	//start metadata setup
	//*********************************

	int buffersize = 1; 
	int write_count = 500;
	if (1 > 0){
		//write path
		int testbuf[4] = {1234, 5678, 9101, 1121};
		int testmetadata[4] = {0, 1, 2, 3};
		//set metadata to start at 0
		//flock(metadata_file, LOCK_EX);
		int temp_test = pwrite(metadata_file, testmetadata, sizeof(int), 0);
        if(temp_test != sizeof(int)){
            printf("failed to write");
            return 0;
	}
	//flock(metadata_file, LOCK_UN);


	//*********************************
	//start application loop 
	//*********************************
	int i = 0;
	uint64_t start_time = usec();
	int prev = -10;
	int prev_write_index = 0;

	while(i < write_count){
		//printf("%d\n", i);
		//if(i % 100000 == 1){
		//	printf("%d\n", i);
		//}
		
		
		//*********************************
		//read metadata loop
		//*********************************
		int index = 0;
		int writeindex = 0;
		while(1){
			//flock(metadata_file, LOCK_EX);
			//read from the metadata file
			int writetest = pread(metadata_file, &(testmetadata[1]), sizeof(int), sizeof(int));
			if(writetest != sizeof(int)){
				printf("failed to read 1");
				return 0;
			}

			index = i; 
			writeindex = testmetadata[1];
			if(writeindex < prev_write_index){
				printf("WENT BACK IN TIME\n");
				return 0;
			}

			prev_write_index = writeindex;
			if(writeindex <= index){
				//wait for writer
				//flock(metadata_file, LOCK_UN);
				continue;
			}else{
				break;
			}
		}




		int readcount = writeindex - index;
		if(readcount <= 0){
			printf("index isn't correct");
			break;
		}


		//************************
		//read from the buffer
		//************************

		int readtest = pread(buffer_file, testbuf, sizeof(int), ((index + 0) % buffersize) * sizeof(int));
		if(readtest != sizeof(int)){
			printf("failed to read 2 value %d\n", readtest);
			printf("file decriptor is %d error %d \n", buffer_file, errno);
			return 0;
			continue;
		}else{
		}

		//************************
		//check that value is correct
		//************************


		if(testbuf[0] != prev + 10){
			printf("FAILED %d after %d index %d file index %d", testbuf[0], prev, index, ((index + 0) % buffersize) * sizeof(int));	

			//flock(buffer_file, LOCK_UN);
			//flock(metadata_file, LOCK_UN);
			return -1;
		}



		//************************
		//update metadata
		//************************

		prev = testbuf[0];
		i += 1;//readcount;
		testmetadata[0] = i;

		//update the metadata path, allowing write path to proceed
		ssize_t metadata_test = pwrite(metadata_file, testmetadata, sizeof(int), 0);
		if(metadata_test != sizeof(int)){
			printf("failed to write");
			return 0;
		}

		//flock(buffer_file, LOCK_UN);
		//flock(metadata_file, LOCK_UN);
	}
	uint64_t end_time = usec();
	uint64_t time_dif = end_time - start_time;

	printf("elapsed time %lu\n", (unsigned long)time_dif);
	printf("tput %f op/s\n",  (write_count - buffersize) / (time_dif / 1e6));
	//writer can write until the difference between them and the reader
	//is at most the length of the file - 1
	}else{
		printf("exiting");
	}
}



