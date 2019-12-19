// Brian Popeck
// Extra Credit for CS1652 Project 1

#include "minet_socket.h"
#include <stdlib.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>


#define FILENAMESIZE 100
#define BUFSIZE      1024

typedef enum { NEW,
	       READING_HEADERS,
	       WRITING_RESPONSE,
	       READING_FILE,
	       WRITING_FILE,
	       CLOSED } states;


struct connection {
    int      sock;
    int      fd;
    char     filename[FILENAMESIZE + 1];
    char     buf[BUFSIZE + 1];
    char   * endheaders;
    bool     ok;
    long     filelen;
    states   state;
    int      headers_read;
    int      response_written;
    int      file_read;
    int      file_written;
    
    struct connection * next;
};

struct sockaddr_in sa;  // server address
int max_minet_fd;   // maximum file descriptor of all active minet sockets
int max_unix_fd;    // maximum file descriptor of all monitored local files


void read_headers  (struct connection * con);
void write_response(struct connection * con);
void read_file     (struct connection * con);
void write_file    (struct connection * con);

int
main(int argc, char ** argv)
{
  
    int     server_port  = 0;
    int     sock         = 0;  
    int     rc           = 0;

    const int BACKLOG = 50;  // number of incoming connections to hold in queue

    /* parse command line args */
    if (argc != 3) {
	fprintf(stderr, "Usage: http_server3 <k|u> <port>\n");
	exit(-1);
    }
    
    server_port = atoi(argv[2]);

    if (server_port < 1500) {
	fprintf(stderr,"INVALID PORT NUMBER: %d; can't be < 1500\n", server_port);
	exit(-1);
    }

    /* initialize and make socket */
    if (toupper(*(argv[1])) == 'K') { 
	minet_init(MINET_KERNEL);
    } else if (toupper(*(argv[1])) == 'U') { 
	minet_init(MINET_USER);
    } else {
	fprintf(stderr, "First argument must be 'k' or 'u'\n");
	exit(-1);
    }
    
    sock = minet_socket(SOCK_STREAM);

    if (sock < 0) {
	minet_perror("couldn't make socket\n");
	exit(-1);
    }

    /* set server address*/
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

    // create list of all connections
    struct connection * open_connection_head = NULL;

    /* connection handling loop */
    // declare the read and write lists
    fd_set minet_read_list;
    fd_set unix_read_list;
    fd_set write_list;
    
    while(1) {
	/* create read and write lists */
    // create three lists: one for reading minet sockets, one for reading local files, one for writing to minet sockets
    FD_ZERO(&minet_read_list);
    FD_ZERO(&unix_read_list);
    FD_ZERO(&write_list);
    max_minet_fd = 0;
    max_unix_fd = 0;

    struct connection * open_connection = open_connection_head;
    while (open_connection != NULL) {
        if (open_connection->state == NEW || open_connection->state == READING_HEADERS) {   // socket needs to start reading or finish reading
            // add to read list
            FD_SET(open_connection->sock, &minet_read_list);
            if (open_connection->sock > max_minet_fd) {
                max_minet_fd = open_connection->sock;
            }
        } else if (open_connection->state == READING_FILE) {    // socket needs to resume reading file
            // add to read list
            FD_SET(open_connection->fd, &unix_read_list);
            if (open_connection->fd > max_unix_fd) {
                max_unix_fd = open_connection->fd;
            }
        } else if (open_connection->state == WRITING_RESPONSE) {    // socket needs to resume writing to client
            // add to write list
            FD_SET(open_connection->sock, &write_list);
            if (open_connection->sock > max_minet_fd) {
                max_minet_fd = open_connection->sock;
            }
        } else if (open_connection->next != NULL && open_connection->next->state == CLOSED) {   // the next connection is closed
            // remove the closed connection from the list of open connections
            struct connection * closed_connection = open_connection->next;
            open_connection->next = closed_connection->next;
            free(closed_connection);
        } else if (open_connection == open_connection_head && open_connection->state == CLOSED) { // SPECIAL CASE: this connection is the only connection, and it is closed
            // remove the closed connection from the front of the list
            open_connection_head = open_connection->next;
            free(open_connection);
        }
        // iterate through the list of open connections
        open_connection = open_connection->next;
    }
    
    // add the accept socket to the read list
    struct connection accept_connection;
    accept_connection.sock = sock;
    if (accept_connection.sock > max_minet_fd) {
        max_minet_fd = accept_connection.sock;
    }
    FD_SET(sock, &minet_read_list);

	/* do a select */
    minet_select_ex(max_minet_fd + 1, &minet_read_list, &write_list, NULL, max_unix_fd + 1, &unix_read_list, NULL, NULL, NULL);

	/* process sockets that are ready */
    int i;
    for (i = 0; i < max_minet_fd + 1; i++) {
        if (FD_ISSET(i, &minet_read_list)) {    // a minet socket is ready for reading
            if (i == sock) {    // the accept socket is ready for reading
                // only block on accept if there are no other sockets to service
                if (open_connection_head == NULL) { // there are no open connections
                    minet_set_blocking(sock);
                } else {    // there are open connections
                    minet_set_nonblocking(sock);
                }

                int accepted_sock = minet_accept(sock, NULL);   // don't need information about client socket
                
                if (accepted_sock >= 0) {   // accepted a new socket
                    minet_set_nonblocking(accepted_sock);   // don't block on reads and writes to socket
                    struct connection * accepted_connection = (connection*) calloc(1, sizeof(struct connection));
                    accepted_connection->sock = accepted_sock;
                    accepted_connection->state = NEW;

                    // add the accepted connection to the list of open connections
                    FD_SET(accepted_sock, &minet_read_list);
                    
                    // insert the new connection at the front of the list
                    accepted_connection->next = open_connection_head;
                    open_connection_head = accepted_connection;
                } else {    // failed to accept a new socket
                    continue;
                }
            } else {    // another minet socket is ready for reading
                // look for the appropriate open connection to read headers from
                open_connection = open_connection_head;
                while (open_connection != NULL) {
                    if (open_connection->sock == i) {
                        read_headers(open_connection);
                        break;
                    }
                    open_connection = open_connection->next;
                }
            }
        } else if (FD_ISSET(i, &unix_read_list)) {  // a local file is ready for reading
            // look for the correct open connection to resume reading for
            open_connection = open_connection_head;
            while (open_connection != NULL) {
                if (open_connection->fd == i) {
                    write_response(open_connection);
                    break;
                }
                open_connection = open_connection->next;
            }
        } else if (FD_ISSET(i, &write_list)) {  // a minet socket is ready for writing
            // look for the correct open connection to resume writing to
            open_connection = open_connection_head;
            while (open_connection != NULL) {
                if (open_connection->fd == i) {
                    write_response(open_connection);
                    break;
                }
                open_connection = open_connection->next;
            }
        }
    }
    }
}

// Reads the headers for con, parsing the header to try and open the correct file. Then initiates writing response 
void
read_headers(connection *con)
{
    con->state = READING_HEADERS;
    int rc = 0;
    /* first read loop -- get request and headers*/
    /* request line and each header terminate in CRLF, and entire request line
    plus header section terminated in CRLF */
    while ((rc = minet_read(con->sock, con->buf + con->headers_read, BUFSIZE - con->headers_read)) > 0) {

    con->headers_read      += rc;  
    con->buf[con->headers_read]  = '\0';

    if ((con->endheaders = strstr(con->buf, "\r\n\r\n")) != NULL) {   
    // we have seen consecutive CRLFs, so we have read all the request line and all the headers
        con->endheaders = con->endheaders + 4;
        break;
    }
    }

    if (rc < 0) {
        if (errno == EAGAIN) {
            // the read has blocked
            return;
        } else {
            perror("error when reading headers: ");
            exit(-1);
            // return;
        }
    }
  
    /* parse request to get file name */
    /* Assumption: this is a GET request and filename contains no spaces*/
  
    /* get file name and size, set to non-blocking */
    /* get name */
    char http_version[20];
    rc = sscanf(con->buf, "GET %s %s", con->filename, http_version);

    if (rc < 0) {
        if (errno == EAGAIN) {  // the read has blocked
            return;
        } else {    // the read has failed for some other reason
            perror("error when parsing headers: ");
            exit(-1);
        }
    }
    /* try opening the file */
    FILE* f = fopen(con->filename, "r");
    if (f == NULL) {    // the file could not be opened
        con->ok = 0;
    } else {
        // configure the file descriptor
        con->fd = fileno(f);
        con->ok = 1;
    }

    /* set to non-blocking, get size */
    if (con->ok) {
        fcntl(con->fd, F_SETFL, O_NONBLOCK);
        struct stat st;
        stat(con->filename, &st);
        con->filelen = st.st_size;

        fclose(f);
    }
    
    write_response(con);
}

// Writes the appropriate response for con to the client.
void
write_response(struct connection * con)
{
    con->state = WRITING_RESPONSE;
    int rc = 0;

    char ok_response_fmt[] = "HTTP/1.0 200 OK\r\n"	               \
	                     "Content-type: text/plain\r\n"            \
	                     "Content-length: %d \r\n\r\n";


    char notok_response[]  = "HTTP/1.0 404 FILE NOT FOUND\r\n"	       \
	                     "Content-type: text/html\r\n\r\n"	       \
	                     "<html><body bgColor=black text=white>\n" \
	                     "<h2>404 FILE NOT FOUND</h2>\n"	       \
	                     "</body></html>\n";

    char ok_response[100];

    /* send response */

    /* send headers */
    if (con->ok) {
        // send the ok header
        sprintf(ok_response, ok_response_fmt, con->filelen);
        int response_headers_len = strlen(ok_response);
        while ((rc = minet_write(con->sock, ok_response, response_headers_len - con->response_written)) > 0) {
            con->response_written += rc;
        }
        if (rc < 0) {
            if (errno == EAGAIN) {  // the write to the socket blocked
                return;
            } else {    // the write to the socket failed for some other reason
                perror("error when writing to socket");
                exit(-1);
            }
        }
    } else {
        // send the not_ok header
        int response_headers_len = strlen(notok_response);
        while ((rc = minet_write(con->sock, notok_response, response_headers_len - con->response_written)) > 0) {
            con->response_written += rc;
        }

        if (rc < 0) {
            if (errno == EAGAIN) {  // the write to the socket blocked
                return;
            } else {
                perror("error when writing to socket");
                exit(-1);
            }
        }
    }
    
    /* Send Data */
    if (con->ok) {
        while (con->file_written < con->filelen) {
            if (con->file_read == con->file_written) {  // have written just as much file content as we have read, ready to read some more
                read_file(con);    
            }
            write_file(con);
        }   
    }
    
    // close the socket and mark the connection as closed
    minet_close(con->sock);
    con->state = CLOSED;
}

// Reads up to BUFSIZE bytes of the file, starting at the beginning of unread data, requested by con into con's buffer
void
read_file(struct connection * con)
{
    con->state = READING_FILE;

    FILE* f = fopen(con->filename, "r");
    fseek(f, con->file_read, SEEK_SET); // seek to where we have yet to read in the file
    if (f == NULL) {
        fprintf(stderr, "the file %s could not be opened for reading\n", con->filename);
        exit(-1);
    }

    int bytes_to_read = con->filelen - con->file_read;
    // can't read more than the buffer at a time
    if (bytes_to_read > BUFSIZE) {  // need to read more data than the buffer can hold
        bytes_to_read = BUFSIZE;
    }
    int rc = fread(con->buf, 1, bytes_to_read, f);

    if (rc > 0) {   // read rc more bytes
        con->file_read += rc;
    } else if (rc == 0) {   // reached the end of the file successfully 
        con->file_read += bytes_to_read;
    } 
    else if (rc < 0) {
        if (errno == EAGAIN) {  // the local file blocked on read
            fclose(f);
            return;
        } else {    // the read blocked for some other reason
            fprintf(stderr, "the file %s could not be read\n", con->filename);
            exit(-1);
        }
    }

    fclose(f);
}

// Writes the bytes read but not yet written to the client socket.
void
write_file(struct connection *con)
{
    con->state = WRITING_FILE;

    int rc = minet_write(con->sock, con->buf, con->file_read - con->file_written);

    if (rc > 0) {
        con->file_written += rc;
    } else if (rc < 0) {
        if (errno == EAGAIN) {  // the minet socket blocked while being written to
            return;
        } else {    // the write blocked for some other reason
            fprintf(stderr, "the socket %d could not be written to\n", con->fd);
            exit(-1);
        }
    }
}
