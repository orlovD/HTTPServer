#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h> //for read()
#include <fcntl.h> //for open flags
#include <sys/mman.h>
#include <sys/time.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <sys/socket.h> /* definitions of structures needed for sockets */
#include <netinet/in.h> /* constants and structures needed for Internet domain addresses */
#include <assert.h> /* asserts */
#include <errno.h> /* error messages */
#include <string.h> /* string functions */
#include <sys/select.h> /* select */
#include <pthread.h> /* for threads */
#include <sys/wait.h> // for wait macros etc

#define MAX_REQUEST_LENGTH 1024
#define SERVICE_UNAVAILABLE "HTTP/1.0 503 OK\r\n\r\n<title>ERROR 503: Service Unavailable!</title><body>ERROR 503: This Service is Not Available!</body>"
#define NOT_IMPLEMENTED "HTTP/1.0 501 OK\r\n\r\n<title>ERROR 501: Not Implemented!</title><body>ERROR 501: This Service is Not Implemented!</body>"
#define NOT_FOUND "HTTP/1.0 404 OK\r\n\r\n<title>ERROR 404: Not Found!</title><body>ERROR 404: This File is Not Found!</body>"
#define DIRECTORY_RESPONSE "HTTP/1.0 200 OK\r\n\r\n<title>This is a Directory</title><body></body>"
#define FILE_RESPONSE "HTTP/1.0 200 OK\r\n\r\n"
#define END_OF_REQUEST "\r\n\r\n"

/**
 * link in singly linked list
 */
struct ListNode;
typedef struct ListNode {
	struct ListNode *next; //pointer to next cell
	int fd; //socket file descriptor
}list_node_t;

/*
 * single linked list
 */
typedef struct LinkedList {
	int size;
	list_node_t *head;
	list_node_t *tail;
}linked_list_t;

linked_list_t* shared_list; //pointer to global linked list, shared between threads
int max_request; //max length of linked list

/* global variable for setting lock on mutual exclusion part of code */
// mutex and conditional variable
pthread_mutex_t sharedElementMutex; // mutex for access to shared "data"
pthread_cond_t  canConsumeCondition; // condition variable to wake up "consumers" of data

// shared predicates and data
int canConsume = 0; //used to signal working threads waiting on condition variable
int sharedElement  = 0; //data shared with working threads
int done = 0; //used to signal working threads they should exit

int ctrlc = 0; //global flag to show to main thread that CTRL+C was pressed

/**
 * handle CTRL+C signal
 * set global variable ctrlc to 1
 * when CTRL+C pressed
 */
static void ctrlcHandler(int signo) {
	ctrlc = 1;
}

/**
 * return pointer to empty linked list
 * return NULL on error
 */
linked_list_t* createLinkedList() {
	linked_list_t *list = (linked_list_t *)malloc(sizeof(linked_list_t));
	if (list == NULL) { //check if malloc succeed
		return NULL; //error in creating list
	}
	list->size = 0;
	list->head = list->tail = NULL;
	return list;
}

/**
 * return pointer to empty link list
 * return NULL on error
 */
list_node_t* createListNode() {
	list_node_t *list_node = (list_node_t *)malloc(sizeof(list_node_t));
	if (list_node == NULL) { //check if malloc succeed
		return NULL; //error in creating list node
	}
	return list_node;
}

/**
 * add new node to linked list
 * return 0 on success
 * return 1 on error
 * on an enqueue operation that exceeds max_request
 * return an error code (-1) to the calling thread
 */
int addToLinkedList(list_node_t* new_list_node) {
	/* check if there is still place in list */
	if (shared_list->size == max_request) {
		return -1; //return -1 when list is full
	}
	/* add new node to shared list */
	/* if shared list is empty */
	if (shared_list->head == NULL && shared_list->tail == NULL) {
		shared_list->head = new_list_node;
		shared_list->tail = new_list_node;
	}
	/* if shared list is not empty add to end of list, after tail */
	else {
		shared_list->tail->next = new_list_node;
		shared_list->tail = new_list_node;
	}
	/* increase shared list size */
	shared_list->size++;
	return 0; //return 0 on success
}

/**
 * remove node from linked list
 * return node removed on success
 * return NULL otherwise
 */
list_node_t* removeFromLinkedList() {
	/* if shared list is empty */
	if (shared_list->head == NULL) {
		return NULL;
	}
	/* if shared list is not empty remove from beginning of list, head */
	else {
		list_node_t* temp_list_node = createListNode();
		if (temp_list_node == NULL) { //check if malloc succeed
			return NULL; //error in creating new node
		}
		temp_list_node = shared_list->head;
		shared_list->head = shared_list->head->next;
		shared_list->size--; //decrease shared list size
		/* if list is empty after removing - set head and tail to NULL */
		if (shared_list->size == 0) {
			shared_list->head = shared_list->tail = NULL;
		}
		return temp_list_node;
	}
}

/**
 * return 0 if its GET or POST request
 * return 1 otherwise
 */
int checkRequest (char* request) {
	int i;
	char str1[4] = "GET";
	str1[3] = '\0';
	char str2[5] = "POST";
	str2[4] = '\0';
	int isNotGet = 0;
	int isNotPost = 0;
	for (i = 0; i < sizeof(str1); i++) {
		if (*(request + i) != str1[i]) {
			isNotGet = 1;
		}
	}
	for (i = 0; i < sizeof(str2); i++) {
		if (*(request + i) != str2[i]) {
			isNotPost = 1;
		}
	}
	return isNotGet && isNotPost; //return 0 if Get or Post
}

/**
 * write all data to socket
 * return 0 on success
 * return 1 otherwise
 */
int writeAll(int connection, char* file_out, int size) {
	/* send all data to client */
	int totalsent = 0;
	int nsent;
	int notwritten = size;// file_info->st_size + 19;
	/* keep looping until nothing left to write*/
	while (notwritten > 0) {
		/* notwritten = how much we have left to write
		 totalsent  = how much we've written so far
		 nsent = how much we've written in last write() call */
		if ((nsent = write(connection, file_out + totalsent, notwritten)) == -1) {
			return 1; //return 1 on error
		}
		totalsent += nsent; //sent
		notwritten -= nsent; //left
	}
	return 0; //return 0 on success
}

/**
 * get requested data
 * return 0 on success
 * return errno if defined
 * return 1 otherwise
 */
int getRequestedData(int connection) {
	int i;
	char words[10][MAX_REQUEST_LENGTH]; //variable for parsing input
	char buffer[MAX_REQUEST_LENGTH];
	/* read all bytes */
	int n;
	/* read all data from client till END_OF_REQUEST */
    /* read data from client into buffer
       block until there's something to read
	   print data to screen every time*/
    while (((n = read(connection, buffer, sizeof(buffer)-1)) > 0) && (strncmp(END_OF_REQUEST, &buffer[n - 4], 4))) {
    	buffer[n] = 0;
    }
	if (n < 0) {
		printf("Error reading data from socket!\n");
		return 1; //return error number
	}
	/* parse input line into words */
	sscanf(buffer, "%s %s %s %s %s %s %s %s %s %s",
		words[0], words[1], words[2], words[3], words[4], words[5],
		words[6], words[7], words[8], words[9]);
	/* not GET or POST - exit*/
	if (checkRequest(words[0])) {
		if (writeAll(connection, NOT_IMPLEMENTED, sizeof(NOT_IMPLEMENTED))) {
			printf("Error sending data to socket!\n");
			return 1; //return 1 on error
		}
		/* close connection */
		if((close(connection) == -1)){ //close connection file
			printf("Error closing connection: %s!\n", strerror(errno));
			return errno; //return error number
		}
		return 0; //end of service for this connection
	}
	/* GET or POST request - continue */
	/* get requested filename/ path */
	char fileName[1024];
	i = 0;
	while (words[1][i] != '\0') {
		fileName[i] = words[1][i];
		i++;
	}
	fileName[i] = '\0';
//	printf("file is %s\n", words[1]);
//	printf("file is %s\n", fileName);
	/* Write a response to the client */
	//try to get requested file size
	struct stat file_info; //for getting size of requested file
	/* error in getting file info */
	if (stat(fileName, &file_info) == -1){
		/* if file not exists - return error 404 */
		if (errno == ENOENT) { //file not exists
			if (writeAll(connection, NOT_FOUND, sizeof(NOT_FOUND))) {
				printf("Error sending data to socket!\n");
				return 1; //return 1 on error
			}
			/* close connection */
			if((close(connection) == -1)){ //close connection file
				printf("Error closing connection: %s!\n", strerror(errno));
				return errno; //return error number
			}
		}
	}
	else { //file exists
			/* directory requested */
		    if(file_info.st_mode & S_IFDIR) {
		    	/* if location is directory - create simple HTTP body listing the directory’s contents */
		    	DIR *dir;
		    	struct dirent *entry;
		    	/* open directory succeed */
		    	if ((dir = opendir(fileName)) != NULL) {
		    		/* send welcome response to browser */
    				if (writeAll(connection, DIRECTORY_RESPONSE, sizeof(DIRECTORY_RESPONSE))) {
    					printf("Error sending data to socket!\n");
    					return 1; //return 1 on error
    				}
		    		/* get all files and directories within directory */
    				while ((entry = readdir(dir)) != NULL) {
    					char temp[256];
    					i = 0;
		    			while (entry->d_name[i] != '\0'){
		    				temp[i] = entry->d_name[i];
		    				i ++;
		    			}
		    			temp[i] = '\n';
		    			/* send directory entry */
	    				if (writeAll(connection, temp, i+1)) {
	    					printf("Error sending data to socket!\n");
	    					return 1; //return 1 on error
	    				}
	    				/* send new line character */
	    				if (writeAll(connection, "<br />", sizeof("<br />"))) {
	    					printf("Error sending data to socket!\n");
	    					return 1; //return 1 on error
	    				}
		    		}
    				/* close directory */
		    		if((closedir (dir) == -1)){ //close directory file
		    			printf("Error closing directory: %s!\n", strerror(errno));
		    			return errno; //return error number
		    		}
		    		/* close connection */
		    		if((close(connection) == -1)){ //close connection file
		    			printf("Error closing connection: %s!\n", strerror(errno));
		    			return errno; //return error number
		    		}
		    	}//end opendir() != NULL if
		    	/* could not open directory */
		    	else { //opendir() failed
		    		printf("Error opening directory: %s!\n", strerror(errno));
					return errno; //return error number
				}
		    }
		    /* file requested */
		    else if(file_info.st_mode & S_IFREG)
		    {
				/* open requested file */
		    	int req;
				if((req = open(fileName, O_RDONLY)) == -1){
					printf("Error opening input file: %s!\n", strerror(errno));
					return errno; //return error number
				}
				/* send all data to client */
				/* send response */
				if (writeAll(connection, FILE_RESPONSE, 19)) {
					printf("Error sending data to socket!\n");
					return 1; //return error number
				}
				/* send requested file */
				char read_buffer[1024]; //temporary buffer for read
				int total_read = 0; //counter of total bytes read
				while(total_read != file_info.st_size) {
					int bytes_read;
					if((bytes_read = read(req, read_buffer, 1024)) == -1) {
						printf("Error reading input file: %s!\n", strerror(errno));
						return errno; //return error number
					}
					if (writeAll(connection, read_buffer, bytes_read)) {
						printf("Error sending data to socket!\n");
						return 1; //return 1 on error
					}
					total_read += bytes_read;
				}
				if((close(req) == -1)){ //close file
					printf("Error closing file: %s!\n", strerror(errno));
					return 1; //return 1 on error
				}
				if((close(connection) == -1)){ //close connection file
					printf("Error closing connection: %s!\n", strerror(errno));
					return 1; //return 1 on error
				}
		    }
		    /* if request is not directory or file */
		    else
		    {
				if (writeAll(connection, NOT_IMPLEMENTED, sizeof(NOT_IMPLEMENTED))) {
					printf("Error sending data to socket!\n");
					return 1; //return 1 on error
				}
				if((close(connection) == -1)){ //close connection file
					printf("Error closing connection: %s!\n", strerror(errno));
					return 1; //return 1 on error
				}
		    }
		}
return 0; //connection served
}

/**
 * serve browser request using threads
 * exit(1) on error
 */
void* serveRequest() {
	int connection;
	/* change CTRL+C behavior */
	//sigaction structs to change SIGINT behavior
  	struct sigaction signal = {{0}}; //struct for ignore signal
  	//init struct for handle signal
  	signal.sa_handler = ctrlcHandler; //exit all threads
  	signal.sa_flags = 0; //bit mask to modify behavior of signal
  	/* apply changes */
	if (sigaction(SIGINT, &signal, NULL) == -1) {
		printf("Error setting new behavior to SIGINT: %s!\n", strerror(errno)); //print error message
		exit(1); //exit on error
	}
//	printf("Consumer Thread %u: Entered\n", pthread_self());
	// initial lock
	if (pthread_mutex_lock(&sharedElementMutex) != 0) {
		printf("Error locking shared element: %s!", strerror(errno));
		exit(1); //exit on error
	}
	// main loop. verifies main thread did not signal to terminate
	// and processes any request in shared list
	while (!done || (shared_list->size != 0)) {
		//printf("Consumer Thread %u: !done || (shared_list->size == 0)\n", pthread_self());
		//printf("done=%d, (shared_list->size)=%d\n", done, (shared_list->size));
	      /* secondary loop, for condition variable
	       * canConsume required for safe use of condition variables.
	       * no data - wait. otherwise - process immediately */
		while (!canConsume) {
//			printf("Consumer Thread %u: Wait for data to be produced\n", pthread_self());
			// note - mutex is already locked here!
			/* conditionally wait */
			if (pthread_cond_wait(&canConsumeCondition, &sharedElementMutex) != 0) {
				printf("Error waiting shared element: %s!", strerror(errno));
				/* unlock shared element */
				if (pthread_mutex_lock(&sharedElementMutex) != 0) {
					printf("Error locking shared element: %s!", strerror(errno));
//					printf("Consumer Thread %u: condwait failed\n", pthread_self());
					exit(1); //exit on error
				}
				exit(1); //exit on error
			}
	         // mutex locked here too!
	         if(done && (shared_list->size == 0)){
	        	 //printf("Consumer Thread %u: done && (shared_list->size == 0) ----break #1\n", pthread_self());
	             break; //exit loop 1
	         }
	      }
	      /* producer thread is done  */
	      if(done && (shared_list->size == 0)){
//	    	  printf("Consumer Thread %u: done && (shared_list->size == 0) ----break #2\n", pthread_self());
//		      printf("Consumer thread %u: signaled to exit gracefuly. exit loop\n", pthread_self());
	          break; //exit loop 2
	      }
//		  printf("Consumer thread %u: Found data! hooray\n", pthread_self());
	      /* remove the shared data */
//		  printf("done=%d, (shared_list->size)=%d\n", done, (shared_list->size));
	      /* remove node from lnked list */
	      list_node_t* current_list_node = removeFromLinkedList();
	      /* We consumed the last of the data */
	      if (shared_list->size == 0){
	          canConsume = 0;
	      }

	      /* unlock shared element */
	      if (pthread_mutex_unlock(&sharedElementMutex) != 0) {
	    	  printf("Error unlocking shared element: %s!", strerror(errno));
	    	  exit(1); //exit on error
	      }

	      /* serve removed FD */
	      connection = current_list_node->fd; //get connection to serve
	      free(current_list_node); //free removed list node
	      /* get requested data for connection */
	      if (getRequestedData(connection)) {
	    	  printf("Error in getting data for connection!\n");
	    	  exit(1); //exit on error
	      }
	      /* lock shared element */
	      if (pthread_mutex_lock(&sharedElementMutex) != 0) {
	    	  printf("Error locking shared element: %s!", strerror(errno));
	    	  exit(1); //exit on error
	      }
	      /* repeat while still holding the lock. pthread_cond_wait releases it atomically */
	   }
//	   printf("Consumer Thread %u: All done\n",pthread_self());
	   /* unlock shared element */
	   if (pthread_mutex_unlock(&sharedElementMutex) != 0) {
		   printf("Error unlocking shared element: %s!", strerror(errno));
		   exit(1); //exit on error
	   }
	   return NULL;
}

/**
 * main thread
 */
int main(int argc, char *argv[]) {
	int i; //index variable
	/* get input arguments */
	int number_of_threads;
	int port_number = 8080; //optional, uses default HTTP if not provided
	/* check if there are enough arguments received */
	if ((argc != 3) && (argc != 4)) {
		printf("Error: Wrong number of arguments received!\n");
		return 1; //terminate program on error
	}
	/* convert input parameters from string to int */
	char* end_ptr;
	number_of_threads = strtol(argv[1], &end_ptr, 10);
	max_request = strtol(argv[2], &end_ptr, 10);
	if (argc == 4) {
		port_number = strtol(argv[3], &end_ptr, 10);
	}
	/* create shared list */
	shared_list = createLinkedList();
	if (shared_list == NULL) {
		printf("Error creating shared linked list!\n");
		return 1; //terminate program on error
	}
	/* setup connection parameters */
	struct sockaddr_in server_address, client_address; /* structure for socket parameters */
	int list_socket; /* listening socket descriptor */
	socklen_t clientLen; /* listening socket size */
	/* create listening socket */
	if ((list_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) { /* create socket of Internet domain, messages read in streams, OS chooses TCP */
		printf("Error creating socket: %s!\n", strerror(errno));
		return errno; //exit on error
	}
	/* set server address parameters */
	memset((char *) &server_address, 0, sizeof(server_address));
	server_address.sin_family = AF_INET; /* code for the address family, always set to the AF_INET */
	server_address.sin_port = htons(port_number); /* port number, a port number in host byte order converted to a port number in network byte order */
	server_address.sin_addr.s_addr = htonl(INADDR_ANY); /* field contains the IP address of the host, for server - IP address of the machine on which the server is running */
	/* reuse server address */
	int yes = 1;
	if ((setsockopt(list_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)) {
		printf("Error reusing port: %s!\n", strerror(errno));
		return errno; //exit on error
	}
	/* bind the socket */
	if ((bind(list_socket, (struct sockaddr *) &server_address, sizeof(server_address))) == -1) {
		printf("Error binding socket: %s!\n", strerror(errno));
		return errno; //exit on error
	}
	/* listen on the created socket */
	if (listen(list_socket, max_request) == -1) {
		printf("Error listening to socket: %s!\n", strerror(errno));
		return errno; //exit on error
	}
	/* initialize mutex object */
	if (pthread_mutex_init(&sharedElementMutex, NULL) != 0) {
		printf("Error initializing mutex object: %s!\n", strerror(errno));
		return 1; //terminate program on error
	}
	/* initialize conditional variable */
	if (pthread_cond_init(&canConsumeCondition, NULL) != 0) {
		printf("Error initializing conditional object: %s!\n", strerror(errno));
		return 1; //terminate program on error
	}
	/* create threads */
	pthread_t* tid; //thread array
	tid = (pthread_t *)malloc(sizeof(pthread_t) * number_of_threads); //allocate an array for threads
	if (tid == NULL) { //check if malloc succeed
		printf("Error creating thread array!\n");
		return 1; //terminate program on error
	}
	/* create thread and pass arguments to it accordingly to mode */
	int thr_status;
	for(i = 0; i < number_of_threads; i++) {
		if ((thr_status = pthread_create(&(tid[i]), NULL, &serveRequest, NULL)) != 0) {
			printf("Error creating new thread: %s!\n", strerror(errno));
			return 1; //terminate program on error
		}
	}
	/* change CTRL+C behavior */
  	//sigaction structs to change SIGINT behavior
  	struct sigaction signal = {{0}}; //struct for ignore signal
  	//init struct for handle signal
  	signal.sa_handler = ctrlcHandler; //exit all threads
  	signal.sa_flags = 0; //bit mask to modify behavior of signal
  	/* apply changes */
	if (sigaction(SIGINT, &signal, NULL) == -1) {
		printf("Error setting new behavior to SIGINT: %s!\n", strerror(errno)); //print error message
		return errno; //return error number and terminate parent process
	}
	/* wait for new clients */
	while (1) {
		int newConnection;
		/* accept client connection */
		clientLen = sizeof(client_address);
		if ((newConnection = accept(list_socket, (struct sockaddr *) &client_address, &clientLen)) == -1) {
			/* if system call was interrupted by signal that was caught before a valid connection arrived */
			if (errno == EINTR) {
				/* if accept() was interrupted by CTRL+C */
				if (ctrlc == 1) {
					/* now wait for all threads and exit when done */
					/* exit all threads when done */
					if (pthread_mutex_lock(&sharedElementMutex) != 0) {
						printf("Error locking mutex element: %s!", strerror(errno));
						return 1; //terminate program on error
					}
					done = 1; //no new requests in the queue
					if (pthread_mutex_unlock(&sharedElementMutex) != 0) {
						printf("Error unlocking mutex element: %s!", strerror(errno));
						return 1; //terminate program on error
					}
					/* send conditional signal to all threads */
					for (i=0; i < number_of_threads; i++) {
						if (pthread_mutex_lock(&sharedElementMutex) != 0) {
							printf("Error locking mutex element: %s!", strerror(errno));
							return 1; //terminate program on error
						}
						if (pthread_cond_signal(&canConsumeCondition) != 0) {
							printf("Error waking up thread: %s!", strerror(errno));
							return 1; //terminate program on error
						}
						if (pthread_mutex_unlock(&sharedElementMutex) != 0) {
							printf("Error unlocking mutex element: %s!", strerror(errno));
							return 1; //terminate program on error
						}
					}
					/* wait for all threads */
					int thr_status;
					for(i = 0; i < number_of_threads; i++) {
						if ((thr_status = pthread_join(tid[i], NULL)) != 0) {
							printf("Error joining threads: %s!\n", strerror(errno));
							exit(1); //exit on error
						}
					}
					if((close(list_socket) == -1)){ //close listening socket
						printf("Error closing connection: %s!\n", strerror(errno));
						return 1; //terminate program on error
					}
					break; //exit loop -> program will go to clean up section
				}
				/* if accept() was interrupted by something else than CTRL+C */
			    else {
			    	printf("Error in accept: %s!\n", strerror(errno));
			    	return errno; //exit on error
			    }
			}
		}
		/* Protect shared data and flag */
		if (pthread_mutex_lock(&sharedElementMutex) != 0) {
			printf("Error locking mutex object: %s!", strerror(errno));
			return 1; //terminate program on error
		}
		/* add data to shared element */
		/* size will increase automatically */
		/* allocate new node before adding to shared list */
		list_node_t* new_list_node = createListNode();
		if (new_list_node == NULL) { //check if malloc succeed
			printf("Error creating new node!\n");
			return 1; //terminate program on error
		}
		/* add FD to node data */
		new_list_node->fd = newConnection;
		/* add request to linked list */
		int add;
		/* if adding to list failed */
		if ((add = addToLinkedList(new_list_node)) != 0) {
			/* if the queue is full, return Service Unavailable (HTTP 503) to the client and close the connection */
			if (add == -1) {
				if (writeAll(newConnection, SERVICE_UNAVAILABLE, sizeof(SERVICE_UNAVAILABLE))) {
					printf("Error sending data to socket!\n");
					return 1; //terminate program on error
				}
				if((close(newConnection) == -1)){ //close connection file
					printf("Error closing connection: %s!\n", strerror(errno));
					return errno; //return error number
				}
			}
			/* if list is not full but adding to list still failed */
			else {
				printf("Error: Adding to list failed!\n");
				return 1; //terminate program on error
			}
		}
		/* Set boolean predicate */
		canConsume = 1;
		/* wake up a consumer  */
		if (pthread_cond_signal(&canConsumeCondition) != 0) {
			printf("Error waking up thread: %s!", strerror(errno));
			return 1; //terminate program on error
		}
		/* unlock shared data */
		if (pthread_mutex_unlock(&sharedElementMutex) != 0) {
			printf("Error unlocking mutex object: %s!", strerror(errno));
			return 1; //terminate program on error
		}
	}
	/* CLEAN UP RESOURCES */
	/* destroy mutex object */
	if (pthread_mutex_destroy(&sharedElementMutex) != 0) {
		printf("Error destroying mutex object: %s!", strerror(errno));
		return 1; //terminate program on error
	}
	/* destroy conditional object */
	if (pthread_cond_destroy(&canConsumeCondition) != 0) {
		printf("Error destroying conditional object: %s!", strerror(errno));
		return 1; //terminate program on error
	}
	/* free allocated resources */
	free(shared_list); //free shared linked list
	free(tid); 	//free thread array
	return 0; //end of program
}///

