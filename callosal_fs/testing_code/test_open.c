#include <stdio.h>
//https://www.thegeekstuff.com/2012/07/c-file-handling/

int main(){
	
	printf("hello world\n");
	
	//open the file 
	FILE * testfile = fopen("test/test", 'w');
	//apparently w will "destroy contents"
	//https://en.cppreference.com/w/c/io/fopen
	//

}
