file_server: file_server.c
	gcc -o file_server file_server.c -pthread
	
clean:
	rm -f *.txt *.o file_server
