#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <search.h>


void die(const char *msg)
{
	perror(msg);
	abort();
}

int restart = 0;				/* pending restart */

static void hup_signal(int sig)
{
	restart = 1;
}

static int recv_fd(int sockfd)
{
	char tmp[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct msghdr msg;
	char ch = '\0';

	memset(&msg, 0, sizeof(msg));
	iov.iov_base = &ch;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = tmp;
	msg.msg_controllen = sizeof(tmp);

	if (recvmsg(sockfd, &msg, 0) <= 0)
		return -1;
	cmsg = CMSG_FIRSTHDR(&msg);
	return *(int *) CMSG_DATA(cmsg);
}

static int send_fd(int sockfd, int fd)
{
	char tmp[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct msghdr msg;
	char ch = '\0';

	memset(&msg, 0, sizeof(msg));
	msg.msg_control = (caddr_t) tmp;
	msg.msg_controllen = CMSG_LEN(sizeof(int));
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	*(int *)CMSG_DATA(cmsg) = fd;
	iov.iov_base = &ch;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (sendmsg(sockfd, &msg, 0) != 1)
		return -1;

	return 0;
}

struct scgi_server
{
	unsigned short listen_port;
	int max_children;
	void *children;				/* binray tree of children */
};

struct child
{
	pid_t pid;
	int fd;
};

fd_set children_fdset;
int highest_fd;

int child_cmp(const void *a, const void *b)
{
	struct child *ca = (struct child *)a;
	struct child *ba = (struct child *)b;
	if (ca->pid < cb->pid)
		return -1;
	else if (ca->pid > cb->pid)
		return 1;
	return 0;
}

void add_child(void *root, pid_t pid, int fd)
{
	struct child *c = malloc(sizeof(struct child));
	c->pid = pid;
	c->fd = fd;

	void *val = tsearch((void *)c, &root, child_cmp);
	if (val == NULL)
		die("add child error");
}

void set_fd(const void *nodep, const VISIT which, const int depth)
{
	struct child *c;

	switch (which) {
	case postorder:
	case leaf:
		c = *(struct child **)nodep;
		FD_SET(c->fd, &children_fdset);
		if (c->fd > highest_fd)
			highest_fd = c->fd;
		break;
	}
}

void fill_children_fdset(void *root)
{
	FD_ZERO(children_fdset);
	highest_fd = -1;
	twalk(root, set_fd);
}

struct *ready_child;

void get_child(const void *nodep, const VISIT which, const int depth)
{
	struct child *c;

	switch (which) {
	case postorder:
	case leaf:
		c = *(struct child **)nodep;
		if (FD_ISSET(c->fd, &children_fdset)) {
			if (!ready_child)
				ready_child = c;
		}
		break;
	}
}

void get_ready_child(void *root)
{
	ready_child = NULL;
	twalk(root, get_child);
}

int spawn_child(struct scgi_server *server, int conn)
{
	int flag = 1;
	int fd[2];	/* parent, child */
	
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0) {
		return -1;
	}

	/* make child fd non-blocking */
	if ((flag = fcntl(fd[1], F_GETFL, 0)) < 0 ||
	    fcntl(fd[1], F_SETFL, flag | O_NONBLOCK) < 0) {
		return -1;
	}

	pid_t pid = fork();
	if (pid == 0) {
		if (conn)
			close(conn);

		close(fd[0]);
		handler_serve(fd[0]);
		exit(0);
	} else if (pid > 0) {
		close(fd[0]);
		add_child(server->children, pid, fd[1]);
	} else {
		perror("fork failed");
		return -1;
	}
	return 0;
}

int delegate_request(struct scgi_server, int conn)
{
	int r;
	struct timeval timeout;

	timeout.tv_usec = 0;
	timeout.tv_sec = 0;

	while (1) {
		fill_children_fdset(scgi_server->children);
		r = select(highest_fd + 1, &children_fdset, NULL, NULL, &timeout);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			} else {
				die("select error");
			}
		} else if (r > 0) {
			/*  One or more children look like they are ready.
			    Do the same walk order so that we keep preferring
			    the same child.
			*/
			get_ready_child(scgi_server->children);
			if (ready_child == NULL) {
				/* should never get here */
				fputs("Ooops\n", stderr);
				continue;	
			}

			/*
			  Try to read the single byte written by the child.
			  This can fail if the child died or the pipe really
			  wasn't ready (select returns a hint only).  The fd has
			  been made non-blocking by spawn_child.  If this fails
			  we fall through to the "reap_children" logic and will
			  retry the select call.
			*/

			if (read(ready_child->fd, buf, 1) != 1) {
				if (errno == EAGAIN) {
					;	/* pass */
				} else {
					/* XXX: pass if child died */
					die("read byte error");
				}
			} else {
				assert(buf[0] == '1');
				/*
				  The byte was read okay, now we need to pass the fd
				  of the request to the child.  This can also fail
				  if the child died.  Again, if this fails we fall
				  through to the "reap_children" logic and will
				  retry the select call.
				*/
				if (send_fd(ready_child->fd, conn) == -1) {
					if (errno != EPIPE) {
						die("sendfd error");
					}
				} else {
					/*
					  fd was apparently passed okay to the child.
					  The child could die before completing the
					  request but that's not our problem anymore.
					*/
					return 0;
				}
			}
		}

		/* didn't find any child, check if any died */
		reap_children(server);

		/* start more children if we haven't met max_children limit */
		if (len(server->children) < server->max_children)
			spawn_child(server);

		/* Start blocking inside select.  We might have reached
		   max_children limit and they are all busy.
		*/
		timeout.tv_sec = 2;

	} /* end of while */

	return 0;
}

int do_restart()
{
	return 0;
}

int serve(struct scgi_server *server)
{
	int flag, sock, conn;
	struct sockaddr_in bindaddr;
	struct sockaddr *cliaddr;
	socklen_t addrlen = sizeof(cliaddr);

	sock = socket(AF_INET, SOCK_STREAM, 0);

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(int)) < 0) {
		perror("setsockopt error");
		return -1;
	}

	bindaddr.sin_family = AF_INET;
	bindaddr.sin_port = htons(server->listen_port);
	bindaddr.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) < 0) {
		perror("bind addr error");
		return -1;
	}

	if (listen(sock, 256) < 0) {
		perror("listen error");
		close(sock);
		return -1;
	}

	signal(SIGHUP, hup_signal);

	while (1) {
		conn = accept(sock, cliaddr, &addrlen);
		if (conn != -1) {
			delegate_request(server, conn);
			close(conn);
		} else if (errno != EINTR) {
			perror("accept error");
			return -1;
		}
		if (restart) {
			do_restart();
		}
	}
	return 0;
}


#define DEFAULT_PORT = 7777
#define MAX_CHILDREN = 10

int main(int argc, char *argv[])
{
	struct scgi_server;
	scgi_server.listen_port = DEFAULT_PORT;
	scgi_server.max_children = MAX_CHILDREN;
	scgi_server.children = NULL;

	serve(&scgi_server);
    	return 0;
}
