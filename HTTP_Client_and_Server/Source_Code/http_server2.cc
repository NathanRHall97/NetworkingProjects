/* Write an HTTP server, http_server2, that avoids just two of these situations: waiting
for a connection to be established, and waiting on the read after a connection has been
established. */

//Server implemented for part 3 of project 1 [CS1652]

#include "minet_socket.h"
#include <stdlib.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>
#include <list>
#include <map>

#define BUFSIZE      1024
#define FILENAMESIZE 100
#define LOADER       15
//#define MAXCONNECTS  32 //defining max connection value


// ---------- STARTING CONNECTION HANDLING CODE HERE ------------------ //

int handle_connection(int sock)
{
    fprintf(stdout, "Handling sock %d \n", sock); //Printing to user that were handling sockets

    // --- INITIATING VARIABLES TO BE USED TO READ AND WRITE IN CONNECTION --- ///
    int ok = 0;
    int fNameSt = 0;
    int fileSize = 0;
    char readIn[BUFSIZE];
    char writeIn[BUFSIZE];
    char *fileName;
    char ch;
    FILE *fp;
    int i = 0;
    int j = 0;
    int n = 0;
    int amount = 0;
    
    char   ok_response_f[]  = "HTTP/1.0 200 OK\r\n"	                \
	                      "Content-type: text/plain\r\n"            \
	                      "Content-length: %d \r\n\r\n";
    char   notok_response[] = "HTTP/1.0 404 FILE NOT FOUND\r\n"	        \
	                      "Content-type: text/html\r\n\r\n"		\
	                      "<html><body bgColor=black text=white>\n"	\
	                      "<h2>404 FILE NOT FOUND</h2>\n"		\
	                      "</body></html>\n";
    //bool   ok = true;

    /* first read loop -- get request and headers*/

    n = minet_read(sock, readIn, BUFSIZE);

    /* parse request to get file name */

    //Setting j back to 0
    j = 0;
    for(i = 0; i < n; i++)  //Go into for lop to read in char bits to file
    {
        if (readIn[i] != ' ' && fNameSt != 0) //reading null, increment, move on
        {
            j++;
        } 
        else if (readIn[i] == ' ' && fNameSt == 0) //reading in
        {
       		if (readIn[i+1] == 47) 
               {
                fNameSt = i+2;
                i++;
                } 
            else {
                fNameSt = i+1;    
            }
        } 
        else if (readIn[i] == ' ' && fNameSt != 0) 
        {
            break;
        } 
        else 
        {
        	continue;
        }
    }
    
    /* try opening the file */
    fileName = (char *)malloc(sizeof(char) * j); //malloc the size of the file chars to make room to open file
    strncpy(fileName, readIn+fNameSt, j); //Copy the string of the file
    fp = fopen(fileName, "r"); //open using r

    if (!fp) 
    {
        fprintf(stdout, "Couldn't open the file\n");
        ok = 0;
    } else {
        ok = 1;
    }

    /* send response */
    if (ok) {
	/* send headers */
		fseek(fp, 0, SEEK_END);
    	fileSize = ftell(fp);
    	fseek(fp, 0, SEEK_SET);
    	sprintf(writeIn, ok_response_f, fileSize);
        amount = 0;
        n = 0; 
        while (amount < strlen(writeIn))  //Writing using strlen with the while loop
        {
            n = minet_write(sock, writeIn + amount, strlen(writeIn) - amount);
            if(n < 0) {
                //perror("Send header failed"); //error handling
                exit(-1);
            }        
            amount += n; //Incr and loop
        }

        /* send file */
		
        n = 0;
		while(1){
			fprintf(stdout, "reading a character\n");
            ch = fgetc(fp);  // get the next character
            writeIn[n++] = ch;
			if (n>=1024 || feof(fp)) {
				fprintf(stdout, "filled the buffer or reached end of file\n");
                int contentInBuffer = n;
                if (feof(fp)) {
                    contentInBuffer -= 1;   // ignore end of file character
                }
                fprintf(stdout, "the content in the buffer is %d bytes long\n", contentInBuffer);
                amount = 0;
		        n = 0;
		        while (amount < contentInBuffer) {
		            n = minet_write(sock, writeIn + amount, contentInBuffer - amount);
		            fprintf(stdout, "wrote %d bytes of the file\n", n);
                    if(n < 0) {
		                //perror("Send file failed");
		                exit(-1);
		            }        
		            amount += n;
		        }				
		        n = 0;		        
				memset(writeIn, 0, sizeof(writeIn)); //Setting memory to write in and the size needed to write
		        if (feof(fp)) break; //Checking a break point edge case
			}
		}

    } else {
        strcpy(writeIn, notok_response); //Responding a not ok write
        amount = 0;
        n = 0;
        while (amount < strlen(writeIn)) {
            n = minet_write(sock, writeIn + amount, strlen(writeIn) - amount);
            if(n < 0) {
                //perror("Send failed");
                exit(-1);
            }        
            amount += n;
        }
    }
    
    if (fp) fclose(fp);
    minet_close(sock); 
    free(fileName);  //free file
  
    if (ok) {
	   return 0;
    } else {
	   return -1;
    }
}

// ---------- STARTING MAIN CODE HERE ------------------ //

//Creating a TCP socken to listen for new connections on
//int fileParse(const char * r, char * fName, int fLength);
struct connection {
    int sock;
};
int main(int argc, char *argv[])
{   
    int server_port = -1;
    int sock        = 0;
    int rc          = -1; //Value in which we listen for new connections

    /* parse command line args */
    if (argc != 3) {
	fprintf(stderr, "usage: http_server1 <k|u> <port>\n");
	exit(-1);
    }

    server_port = atoi(argv[2]);

    if (server_port < 1500) {
	fprintf(stderr, "INVALID PORT NUMBER: %d; can't be < 1500\n", server_port);
	exit(-1);
    }

    
    if (toupper(*(argv[1])) == 'K' || toupper(*(argv[1])) == 'k') 
    {
	    minet_init(MINET_KERNEL);
    } 
    else if (toupper(*(argv[1])) == 'U' || toupper(*(argv[1])) == 'u') 
    {
	    minet_init(MINET_USER);
    } 
    else 
    {
	    fprintf(stderr, "First argument must be 'K' or 'U'\n");
	    exit(-1);
    }
    
    /* initialize and make socket */
    sock = minet_socket(SOCK_STREAM);
    if (sock < 0) {
	//minet_perror(stderr, "couldn't make socket\n");
	exit(-1);
    }
    else
    {
        fprintf(stdout, "Init Socket. \n"); //Says the socket is initialized
    }

    //Adding in code
    /* set server address*/
    struct sockaddr_in sAddress;
    memset(&sAddress, 0, sizeof(sAddress));
    sAddress.sin_port = htons(server_port);
    sAddress.sin_addr.s_addr = INADDR_ANY;
    sAddress.sin_family = AF_INET;

    /* bind listening socket */
    if(minet_bind(sock, &sAddress) < 0) 
    {
        fprintf(stderr, "Binding socket error \n");
        //minet_printer(NULL);
        exit(-1);
    }
    
    /* start listening */
    //keep somaxconn = 128
    if(minet_listen(sock, SOMAXCONN) < 0) //Goes in if there is a socket error
    {
        fprintf(stderr, "Error listening to socket \n");
        //minet_perror(NULL);
        exit(-1);
    }
    else //Socket is correctly listening
    {
        fprintf(stdout, "Listening on port %d\n", server_port);
    }

     /* -------- connection handling loop -------- */ 
    
    /* create read list */
    //Reading list of sockets
    //initalizing reading variables for loop
    fd_set main;
    fd_set listConnections;
    FD_ZERO(&main);
    FD_ZERO(&listConnections);
    FD_SET(sock, &main);
    int maximumSock = sock;
    
    while (1) 
    {
        listConnections = main;
        int selector = minet_select(maximumSock +1, &listConnections, 0,0,0);

        /* poll with select (or poll/epoll) */

        if(selector > 0)
        {
            /* process sockets that are ready */
            for(int i = 0; i < maximumSock+1; i++)
            {
                if(FD_ISSET(i, &listConnections))
                {
                    /* for the accept socket, add accepted connection to connections */
                    if (i == sock)
                    {
                        int open = minet_accept(sock, 0);
                        if(open > 0)
                        {
                            if(open > maximumSock)
                            {
                                maximumSock = open;
                            }
                        FD_SET(open, &main);
                        }
                        else
                        {
                            continue;
                        }
                    }
                }
                else
                {
                    rc = handle_connection(i);
                    FD_CLR(i, &main);
                }
            }

        }
    }
}