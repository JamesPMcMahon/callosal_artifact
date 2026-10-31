## Ring Buffer test 

This test is for a simple ring buffer within a filesystem.\
The ring buffer creates two files, one that stores the current index of the producer and the consumer, and another file that contains
the actual ring buffer itself.\
<br>
The benchmark can be configured by changing the buffer size and the write\_count variables \
<br>
NOTE: On Collosal-FS the metadata and ring buffer files should be created on the reader node manually in the same
order that it is created on the writer node (helps to precreate them). Otherwise their inode numbers will be swapped and they
wont be properly synced \ 
<br>
Compile with ``` gcc -O3 -Wall -g ``` \ 

