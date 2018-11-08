#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <assert.h>

#define DEVICE1 "/dev/simple_char_dev1"
#define DEVICE2 "/dev/simple_char_dev2"

int main(){

	/* make 2 character devices for testing */
	mknod(DEVICE1, S_IFCHR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, makedev(250, 0));
	mknod(DEVICE2, S_IFCHR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, makedev(250, 0));

	/* try to open first device */
	int fd1 = open(DEVICE1, O_RDONLY);
	assert(fd1>=0); //openning should success
	
	/* try to open second device */
	int fd2 = open(DEVICE2, O_RDONLY);
	assert(fd2<0); //openning should fail

	/* close first device */
	assert(!close(fd1));

	/* try to open second device now */
	fd2 = open(DEVICE2, O_RDONLY);
	assert(fd2>=0); //openning should success

	/* close second device */
	assert(!close(fd2));

	return 0;
}///
