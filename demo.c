#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "scgi.h"

#define DEFAULT_PORT  4000
#define MAX_CHILDREN  10

void handler_serve(struct scgi_handler *handler)
{
	char magic = '1';
	int conn;

	while (1) {
		if (write(handler->parent_fd, &magic, 1) < 0) {
			perror("write magic byte error");
			exit(1);
		}

                conn = recv_fd(handler->parent_fd);
		if (conn == -1) {
			perror("recv_fd error");
			exit(1);
		}

		handler->handle_connection(conn);
	}
}

void handle_connection(int conn)
{
	read_env(conn);

	char status[] = "HTTP/1.1 200 OK\r\n";
	char header[] ="Content-Length: 7\r\n";
	char end[] = "\r\n";
	char body[] = "fuckgfw";

	if (write(conn, status, strlen(status)) <= 0) {
		fputs("write error\n", stderr);
	}

	if (write(conn, header, strlen(header)) <= 0) {
		fputs("write error\n", stderr);
	}

	if (write(conn, end, 2) <= 0) {
		fputs("write error\n", stderr);
	}

	if (write(conn, body, strlen(body)) <= 0) {
		fputs("write error\n", stderr);
	}

	close(conn);
}

int main(int argc, char *argv[])
{
	struct scgi_server server;
	struct scgi_handler handler = {
		.serve = handler_serve,
		.handle_connection = handle_connection,
	};

	init_server(&server, DEFAULT_PORT, MAX_CHILDREN, &handler);
	serve_scgi(&server);

    	return 0;
}
