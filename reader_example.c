#include <fcntl.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pipey_ioctl.h"

#define BUFFER_SIZE 1024

void main(int argc, char **argv)
{
	int pipe_fd;
	int file_fd;
	char *mesg = NULL;
	int readed_from_file;
	char *buffer;
	
	if (argc != 2) {
		mesg = "Please, specify which file I should read.\n";
		write(1, mesg, strlen(mesg));
		exit(1);
	}
	
	pipe_fd = open("/dev/pipey", O_WRONLY);
	if (pipe_fd < 0) {
		perror("ERROR!! Failed to open pipe");
		exit(2);
	}
	file_fd = open(argv[1], O_RDONLY);
	if (file_fd < 0) {
		perror("ERROR!! Failed to open file to read from");
		exit(3);
	}
	
	ioctl(pipe_fd, PIPEY_SET_EXCL_READ);
	buffer = (char *)calloc(BUFFER_SIZE, sizeof(char));
	while (1) {
		memset(buffer, 0, BUFFER_SIZE);
		readed_from_file = read(file_fd, buffer, BUFFER_SIZE);
		if (write(pipe_fd, buffer, readed_from_file) <= 0) {
			perror("Error while writing to a pipe");
			exit(4);
		}
		if (readed_from_file < BUFFER_SIZE) {
			printf("END OF FILE\n");
			break;
		}
	}
	
	close(file_fd);
	close(pipe_fd);
	
	exit(0);
}
