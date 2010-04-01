#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "scgi.h"


static void die(const char *msg)
{
	perror(msg);
	abort();
}

int recv_fd(int sockfd)
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
	return *(int *)CMSG_DATA(cmsg);
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

void init_server(struct scgi_server *server, unsigned short port,
		 int max_children, struct scgi_handler *handler)
{
	server->listen_port = port;
	server->max_children = max_children;

	server->children.first = NULL;
	server->children.last = &server->children.first;
	server->children.size = 0;

	server->handler = handler;
}

static void add_child(struct children *children, pid_t pid, int fd)
{
	struct child *c = (struct child *)malloc(sizeof(struct child));
	c->pid = pid;
	c->fd = fd;
	c->next = NULL;

	*(children->last) = c;
	children->last = &(c->next);
	children->size++;
}

static void remove_child(struct children *children, struct child *child)
{
	struct child *c;

	if (children->first == child) {
		if ((children->first = children->first->next) == NULL)
			children->last = &children->first;
	} else {
		c = children->first;
		while (c->next != child)
			c = c->next;

		if ((c->next = c->next->next) == NULL)
			children->last = &c->next;
	}
	children->size--;
	free(child);
}

static int fill_children_fdset(struct children *children, fd_set *fds)
{
	struct child *c;
	int highest_fd = -1;

	FD_ZERO(fds);

	for (c = children->first; c; c = c->next) {
		FD_SET(c->fd, fds);
		if (c->fd > highest_fd)
			highest_fd = c->fd;
	}

	return highest_fd;
}

static struct child *get_ready_child(struct children *children, fd_set *fds)
{
	struct child *c = NULL;

	for (c = children->first; c; c = c->next)
		if (FD_ISSET(c->fd, fds))
			break;

	return c;
}

static struct child *get_child(struct children *children, pid_t pid)
{
	struct child *c = NULL;

	for (c = children->first; c; c = c->next)
		if (c->pid == pid)
			break;

	return c;
}

static int spawn_child(struct scgi_server *server, int conn)
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
		/* in the midst of handling a request,
		   close the connection in the child
		*/
		if (conn)
			close(conn);

		close(fd[1]);
		server->handler->parent_fd = fd[0];
		server->handler->serve(server->handler);
		exit(0);
	} else if (pid > 0) {
		close(fd[0]);
		add_child(&server->children, pid, fd[1]);
	} else {
		perror("fork failed");
		return -1;
	}
	return 0;
}

static void reap_children(struct scgi_server *server)
{
	pid_t pid;
	struct child *child;

	while (server->children.size) {
		pid = waitpid(-1, NULL, WNOHANG);
		if (pid <= 0)
			break;

		child = get_child(&server->children, pid);
		close(child->fd);
		remove_child(&server->children, child);
	}
}

int restart = 0;				/* pending restart */

static void hup_signal(int sig)
{
	restart = 1;
}

static void do_stop(struct scgi_server *server)
{
	struct child *c;

	/* Close connections to the children, which will cause them to exit
	   after finishing what they are doing.	*/
	for (c = server->children.first; c; c = c->next)
		close(c->fd);
}

static void do_restart(struct scgi_server *server)
{
	do_stop(server);
	restart = 0;
}

static int delegate_request(struct scgi_server *server, int conn)
{
	fd_set fds;
	int highest_fd, r;
	struct child *child;
	struct timeval timeout;
	char magic;

	timeout.tv_usec = 0;
	timeout.tv_sec = 0;

	while (1) {
		highest_fd = fill_children_fdset(&server->children, &fds);
		r = select(highest_fd + 1, &fds, NULL, NULL, &timeout);
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
			child = get_ready_child(&server->children, &fds);
			if (child == NULL) {
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

			if (read(child->fd, &magic, 1) != 1) {
				if (errno == EAGAIN) {
					;	/* pass */
				} else {
					/* XXX: pass if child died */
					die("read byte error");
				}
			} else {
				assert(magic == '1');
				/*
				  The byte was read okay, now we need to pass the fd
				  of the request to the child.  This can also fail
				  if the child died.  Again, if this fails we fall
				  through to the "reap_children" logic and will
				  retry the select call.
				*/
				if (send_fd(child->fd, conn) == -1) {
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
		if (server->children.size < server->max_children)
			spawn_child(server, conn);

		/* Start blocking inside select.  We might have reached
		   max_children limit and they are all busy.
		*/
		timeout.tv_sec = 2;

	} /* end of while */

	return 0;
}

int serve_scgi(struct scgi_server *server)
{
	int flag, sock, conn;
	struct sockaddr_in bindaddr;
	struct sockaddr_in cliaddr;
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
		conn = accept(sock, &cliaddr, &addrlen);
		if (conn != -1) {
			delegate_request(server, conn);
			close(conn);
		} else if (errno != EINTR) {
			perror("accept error");
			return -1;
		}
		if (restart) {
			do_restart(server);
		}
	}
	return 0;
}

