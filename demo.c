#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "scgi.h"

#define DEFAULT_PORT  4000
#define MAX_CHILDREN  10


void handle_connection(int conn)
{
	char status[] = "HTTP/1.1 200 OK\r\n";
	char header[] ="Content-Length: 7\r\n";
	char end[] = "\r\n";
	char body[] = "fuckgfw";
	
	/* read headers into environment */
	read_env(conn);
	
	fprintf(stderr, "%s %s %s\n",
		getenv("REMOTE_ADDR"),
		getenv("REQUEST_METHOD"),
		getenv("REQUEST_URI"));
	
	int r;
	
	r = write(conn, status, strlen(status));
	r = write(conn, header, strlen(header));
	r = write(conn, end, 2);
	r = write(conn, body, strlen(body));

	close(conn);
}

int main(int argc, char *argv[])
{
	struct scgi_server server;
	struct scgi_handler handler = {
		.child_init_hook = NULL,
		.handle_connection = handle_connection,
	};

	init_scgi(&server, DEFAULT_PORT, MAX_CHILDREN, &handler);
	serve_scgi(&server);

    	return 0;
}
