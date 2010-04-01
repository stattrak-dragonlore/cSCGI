#ifndef SCGI_H
#define SCGI_H

struct child {
	pid_t pid;
	int fd;
	struct child *next;
};

struct children {
	struct child *first;
	struct child **last;			/* addr of last next element */
	int size;
};

struct scgi_handler {
	int parent_fd;
	void (*serve)(struct scgi_handler *this);
	void (*handle_connection)(int conn);
};

struct scgi_server {
	unsigned short listen_port;
	int max_children;
	struct children children;
	struct scgi_handler *handler;
};


int recv_fd(int sockfd);

void read_env(int conn);

void init_server(struct scgi_server *server, unsigned short port,
		 int max_children, struct scgi_handler *handler);

int serve_scgi(struct scgi_server *server);

#endif
