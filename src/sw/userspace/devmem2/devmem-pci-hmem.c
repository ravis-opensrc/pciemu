#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/mman.h>
  
#define FATAL do { fprintf(stderr, "Error at line %d, file %s (%d) [%s]\n", \
  __LINE__, __FILE__, errno, strerror(errno)); exit(1); } while(0)
 
#define MAP_SIZE 4096UL
#define MAP_MASK (MAP_SIZE - 1)

int main(int argc, char **argv) {
    int fd;
    void *map_base, *virt_addr; 
	unsigned long read_result, writeval;
	unsigned long long read_result_64, writeval_64;
	off_t target;
	int access_type = 'w';
	
	if(argc < 2) {
		fprintf(stderr, "\nUsage:\t%s { address } [ type [ data ] ]\n"
			"\taddress : memory address to act upon\n"
			"\ttype    : access operation type : [b]yte, [h]alfword, [w]ord, [p]doubleword\n"
			"\tdata    : data to be written\n\n",
			argv[0]);
		exit(1);
	}
	target = strtoul(argv[1], 0, 0);

	if(argc > 2)
		access_type = tolower(argv[2][0]);


    if((fd = open("/dev/pci-hmem", O_RDWR | O_SYNC)) == -1) FATAL;
    
    /* Map one page */
    map_base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, target & ~MAP_MASK);
    if(map_base == (void *) -1) FATAL;
    
    virt_addr = map_base + (target & MAP_MASK);
    switch(access_type) {
		case 'b':
			read_result = *((unsigned char *) virt_addr);
			break;
		case 'h':
			read_result = *((unsigned short *) virt_addr);
			break;
		case 'w':
			read_result = *((unsigned long *) virt_addr);
			break;
		case 'p':
			read_result_64 = *((unsigned long long *) virt_addr);
			break;
		default:
			fprintf(stderr, "Illegal data type '%c'.\n", access_type);
			exit(2);
	}

	if(argc > 3) {
		if (access_type == 'p') {
			writeval_64 = strtoull(argv[3], 0, 0);
		} else {
			writeval = strtoul(argv[3], 0, 0);
		}
		switch(access_type) {
			case 'b':
				*((unsigned char *) virt_addr) = writeval;
				read_result = *((unsigned char *) virt_addr);
				break;
			case 'h':
				*((unsigned short *) virt_addr) = writeval;
				read_result = *((unsigned short *) virt_addr);
				break;
			case 'w':
				*((unsigned long *) virt_addr) = writeval;
				read_result = *((unsigned long *) virt_addr);
				break;
			case 'p':
				*((unsigned long long *) virt_addr) = writeval_64;
				read_result_64 = *((unsigned long long *) virt_addr);
				break;
		}
		if (access_type == 'p') {
			printf("Wrote value 0x%llx at address 0x%X and readback 0x%llX\n", writeval_64, target, read_result_64); 
		} else {
			printf("Wrote Value 0x%x at address 0x%X and readback 0x%X\n", writeval, target, read_result); 
		}
		fflush(stdout);
	}
	else {
		if (access_type == 'p') {
			printf("Value at address 0x%X 0x%llX\n", target, read_result_64); 
		} else {
			printf("Value at address 0x%X 0x%X\n", target, read_result); 
		}
		fflush(stdout);
	}
	
	if(munmap(map_base, MAP_SIZE) == -1) FATAL;
            close(fd);

    return 0;
}
