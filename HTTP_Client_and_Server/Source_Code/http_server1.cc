// Brian Popeck
// Part 2 of CS 1652 Project 1

#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "minet_socket.h"

#define BUFSIZE      1024
#define FILENAMESIZE 100

int handle_connection(int);

int writenbytes(int, char *, int);
int readnbytes (int, char *, int);

int
main(int argc, char ** argv)
{

    int server_port = -1;
    int sock        =  0;
    int rc          = -1;

    struct sockaddr_in sa;  // server address
    struct sockaddr_in client_sa;   // information about accepted connection

    const int BACKLOG = 50;  // number of incoming connections to hold in queue

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

    /* initialize and make socket */
    if (toupper(*(argv[1])) == 'K') {
	rc = minet_init(MINET_KERNEL);
    } else if (toupper(*(argv[1])) == 'U') {
	rc = minet_init(MINET_USER);
    } else {
	fprintf(stderr, "First argument must be k or u\n");
	exit(-1);
    }

    if (rc == -1) {
	fprintf(stderr, "Could not initialize Minet\n");
	exit(-1);
    }

    sock = minet_socket(SOCK_STREAM);

    if (sock < 0) {
	minet_perror("couldn't make socket\n");
	exit(-1);
    }

    /* set server address*/
    // see "the old way" at http://beej.us/guide/bgnet/html/multi/syscalls.html#bind
    // use the old way since minet_connect accepts a sockaddr_in struct
    memset(&sa, 0, sizeof(sa));
    sa.sin_addr.s_addr = INADDR_ANY;  // bind to this machine's local IP address
    sa.sin_family = AF_INET;    // accept IPv4 only

    /* bind listening socket */
    sa.sin_port = htons(server_port);   // bind to the specified port on this machine
    rc = minet_bind(sock, &sa);

    if (rc < 0) {
        fprintf(stderr, "could not bind to port %d\n", server_port);
        minet_error();
        exit(-1);
    }

    /* start listening */
    rc = minet_listen(sock, BACKLOG);

    if (rc < 0) {
        minet_perror("couldn't listen on socket ");
        exit(-1);
    }

    /* connection handling loop */
    while(1)
    {
        /* Accept connections */
        int connection_sock;
        connection_sock = minet_accept(sock, &client_sa);

        if (connection_sock < 0) {
            minet_perror("failed to accept client connection\n");
            exit(-1);
        }
      
        rc = handle_connection(connection_sock);

        if (rc < 0) {
            fprintf(stderr, "failed to handle the connection\n");
            exit(-1);
        }

    }

}

// Handles the connection made to sock2 by executing the GET request.
// Sends a file not found message to client if the file path is not in the current directory.
// Returns 0 on success, -1 otherwise. A file not found message is considered a success.
int
handle_connection(int sock2)
{
    char   ok_response_f[]  = "HTTP/1.0 200 OK\r\n"           \
	                      "Content-type: text/plain\r\n"  \
	                      "Content-length: %d \r\n\r\n";
    char   notok_response[] = "HTTP/1.0 404 FILE NOT FOUND\r\n" \
	                      "Content-type: text/html\r\n\r\n"		     \
	                      "<html><body bgColor=black text=white>\n"	     \
	                      "<h2>404 FILE NOT FOUND</h2>\n"
	                      "</body></html>\n";
    
    bool   ok               = false;

    int rc = -1;
    int datalen = 0;

    char* buf = (char *)calloc(1, BUFSIZE);
    char* end_of_headers = NULL;    // end of request line and headers

    char request_uri[FILENAMESIZE + 1], http_version[20];
    char* response_header = NULL;   // pointer to the ok_response header containing the accurate file size

    
    /* first read loop -- get request and headers*/
    /* request line and each header terminate in CRLF, and entire request line
    plus header section terminated in CRLF */
    while ((rc = minet_read(sock2, buf + datalen, BUFSIZE - datalen)) > 0) {
    datalen      += rc;  
    buf[datalen]  = '\0';

    if ((end_of_headers = strstr(buf, "\r\n\r\n")) != NULL) {   
    // we have seen consecutive CRLFs, so we have read all the request line and all the headers
        end_of_headers += 4;
        break;
    }
    }

    /* parse request to get file name */
    /* Assumption: this is a GET request and filename contains no spaces*/
    rc = sscanf(buf, "GET %s %s", request_uri, http_version);

    if (rc < 0) {
        fprintf(stderr, "failed to parse the request and headers\n");
        fprintf(stderr, "%s\n", buf);
        exit(-1);
    }

    
    /* try opening the file */
    FILE* f = fopen(request_uri, "r");
    if (f != NULL) {
        // the file was found
        ok = true;
    } else {
        // the file was not found
        ok = false;
    }

    /* send response */
    if (ok) {
      
      /* send headers */
        // get file size
        struct stat st;
        stat(request_uri, &st);
        off_t file_size = st.st_size;

        // copy response header with correct file size to buffer
        asprintf(&response_header, ok_response_f, file_size);
        rc = writenbytes(sock2, response_header, strlen(response_header));

        if (rc < 0) {
            fprintf(stderr, "the response header could not be written to socket\n");
            exit(-1);
        }


      /* send file */
        int datalen = 0;    // total characters written from file to socket
        // write from file to buffer to socket in loop until whole file has been transferred
        while (datalen < file_size) {
            // read from file
            rc = fread(buf, 1, BUFSIZE, f);
            if (rc < 0) {
                fprintf(stderr, "the file %s could not be read\n", request_uri);
                exit(-1);
            }

            // write to socket
            int wc = writenbytes(sock2, buf, rc);
            if (wc < rc) {
                fprintf(stderr, "failed to write file data to the socket\n");
                exit(-1);
            }

            // update data written count
            datalen += wc;

            // clear the buffer
            memset(buf, 0, BUFSIZE);
        }

        fclose(f);

    } else {
      /* Send error Message */
        rc = writenbytes(sock2, notok_response, strlen(notok_response));
        if (rc < 0) {
            fprintf(stderr, "couldn't write response to client\n");
            exit(-1);
        }
        ok = true;  // server successfully communicated that file is not found - keep running
    }

    /* close socket and free space */
    free(buf);
    minet_close(sock2);

    if (ok) {
	return 0;
    } else {
	return -1;
    }
}

// Writes count bytes from buf to the minet socket with the file descriptor fd.
// Returns 0 on success, -1 otherwise.
int
writenbytes(int    fd,
          char * buf,
          int    count)
{
    int rc           = 0;
    int totalwritten = 0;

    while ((rc = minet_write(fd, buf + totalwritten, count - totalwritten)) > 0) {
    totalwritten += rc;
    }
    
    if (rc < 0) {
    return -1;
    } else {
    return totalwritten;
    }
}
