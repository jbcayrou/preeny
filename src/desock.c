#define _GNU_SOURCE

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <poll.h>

#include "logging.h"

#define PREENY_MAX_FD 8192
#define PREENY_SOCKET_OFFSET 500
#define READ_BUF_SIZE 65536

#define PREENY_SOCKET(x) (x+PREENY_SOCKET_OFFSET)

int preeny_desock_shutdown_flag = 0;
pthread_t *preeny_socket_threads_to_front[PREENY_MAX_FD] = { 0 };
pthread_t *preeny_socket_threads_to_back[PREENY_MAX_FD] = { 0 };
int preeny_original_socket_to_front[PREENY_MAX_FD] = { 0 }; // For port filtering feature

// By default (selected_port=-1), all sockets are forwarded in stdin/out
// If a port is set, only socket associated with it will be forwarded, others use original socket functions
int selected_port = -1; // -1 when all sockets are intercepted
int selected_fd = -1; // For filtering,its value change when a fd socket match the selected port
pthread_mutex_t lock; // To protected selected_fd

int preeny_socket_sync(int from, int to, int timeout)
{
	struct pollfd poll_in = { from, POLLIN, 0 };
	char read_buf[READ_BUF_SIZE];
	int total_n;
	char error_buf[1024];
	int n;
	int r;

	r = poll(&poll_in, 1, timeout);
	if (r < 0)
	{
		strerror_r(errno, error_buf, 1024);
		preeny_debug("read poll() received error '%s' on fd %d\n", error_buf, from);
		return 0;
	}
	else if (poll_in.revents == 0)
	{
		preeny_debug("read poll() timed out on fd %d\n", from);
		return 0;
	}

	total_n = read(from, read_buf, READ_BUF_SIZE);
	if (total_n < 0)
	{
		strerror_r(errno, error_buf, 1024);
		preeny_info("synchronization of fd %d to %d shutting down due to read error '%s'\n", from, to, error_buf);
		return -1;
	}
	preeny_debug("read %d bytes from %d (will write to %d)\n", n, from, to);

	n = 0;
	while (n != total_n)
	{
		r = write(to, read_buf, total_n - n);
		if (r < 0)
		{
			strerror_r(errno, error_buf, 1024);
			preeny_info("synchronization of fd %d to %d shutting down due to read error '%s'\n", from, to, error_buf);
			return -1;
		}
		n += r;
	}

	preeny_debug("wrote %d bytes to %d (had read from %d)\n", total_n, to, from);
	return total_n;
}

__attribute__((destructor)) void preeny_desock_shutdown()
{
	int i;
	int to_sync[PREENY_MAX_FD] = { };

	preeny_debug("shutting down desock...\n");
	preeny_desock_shutdown_flag = 1;


	for (i = 0; i < PREENY_MAX_FD; i++)
	{
		if (preeny_socket_threads_to_front[i])
		{
			preeny_debug("sending SIGINT to thread %d...\n", i);
			pthread_join(*preeny_socket_threads_to_front[i], NULL);
			pthread_join(*preeny_socket_threads_to_back[i], NULL);
			preeny_debug("... sent!\n");
			to_sync[i] = 1;
		}
	}

	for (i = 0; i < PREENY_MAX_FD; i++)
	{
		if (to_sync[i])
		{
			//while (preeny_socket_sync(0, PREENY_SOCKET(i), 10) > 0);
			while (preeny_socket_sync(PREENY_SOCKET(i), 1, 0) > 0);
		}
	}

	preeny_debug("... shutdown complete!\n");
}

void preeny_socket_sync_loop(int from, int to)
{
	char error_buf[1024];
	int r;
	int reforwarded = 0; // To remember if fd forwarding already changed 

	preeny_debug("starting forwarding from %d to %d!\n", from, to);

	while (!preeny_desock_shutdown_flag)
	{

		if (selected_port !=-1 && reforwarded==0)
		{
			// Save previous fowarding of debug print
			int old_from = from;
			int old_to = to;
			
			int * prenny_fd = &from;
			int * std_fd = &to; // stdin/out fd

			if (*prenny_fd < 2){ // stdin= 0, stdout = 1
				prenny_fd = &to;
				std_fd = &from;
			}

			int unprenny_fd = *prenny_fd - PREENY_SOCKET_OFFSET;
			int orig = preeny_original_socket_to_front[unprenny_fd]; // Get the real socket to change forwarding
			pthread_mutex_lock(&lock); // Protect selected_fd access
			int tmp_selected_fd = selected_fd;
			pthread_mutex_unlock(&lock);

			// If filtering is enabled (selected_port!=-1) and re forward all fd not bind on selected_port
			if (tmp_selected_fd != -1 && unprenny_fd != tmp_selected_fd){
				// When fd must be be interecepted
				*std_fd = orig; // Update fowarding
				preeny_debug("Change forwarding (before %d -> %d) now from %d to %d!\n", old_from, old_to, from, to);
			}
			else{
				// unprenny_fd corresponds to fd with the port to intercept.
				reforwarded=1;
			}
		}

		r = preeny_socket_sync(from, to, 15);
		if (r < 0) return;
	}
}

#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"

void *preeny_socket_sync_to_back(void *fd)
{
	int front_fd = (int)fd;
	int back_fd = PREENY_SOCKET(front_fd);
	preeny_socket_sync_loop(back_fd, 1);
	return NULL;
}

void *preeny_socket_sync_to_front(void *fd)
{
	int front_fd = (int)fd;
	int back_fd = PREENY_SOCKET(front_fd);
	preeny_socket_sync_loop(0, back_fd);
	return NULL;
}

//
// originals
//
int (*original_socket)(int, int, int);
int (*original_bind)(int, const struct sockaddr *, socklen_t);
int (*original_listen)(int, int);
int (*original_accept)(int, struct sockaddr *, socklen_t *);
int (*original_connect)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int (*original_close)(int);

__attribute__((constructor)) void preeny_desock_orig()
{
	original_socket = dlsym(RTLD_NEXT, "socket");
	original_listen = dlsym(RTLD_NEXT, "listen");
	original_accept = dlsym(RTLD_NEXT, "accept");
	original_bind = dlsym(RTLD_NEXT, "bind");
	original_connect = dlsym(RTLD_NEXT, "connect");
	original_close = dlsym(RTLD_NEXT, "close");

	// If PORT env var is set, only forward it
	char *port_str = getenv("PORT");
	int port = port_str ? atoi(port_str) : -1;
	selected_port = port;

    pthread_mutex_init(&lock, NULL);
}

int socket(int domain, int type, int protocol)
{
	int fds[2];
	int front_socket;
	int back_socket;

	if (domain != AF_INET && domain != AF_INET6)
	{
		preeny_info("Ignoring non-internet socket.");
		return original_socket(domain, type, protocol);
	}
	
	int r = socketpair(AF_UNIX, type, 0, fds);
	preeny_debug("Intercepted socket()!\n");

	if (r != 0)
	{
		perror("preeny socket emulation failed:");
		return -1;
	}

	preeny_debug("... created socket pair (%d, %d)\n", fds[0], fds[1]);

	front_socket = fds[0];
	back_socket = dup2(fds[1], PREENY_SOCKET(front_socket));
	close(fds[1]);

	preeny_debug("... dup into socketpair (%d, %d)\n", fds[0], back_socket);

	preeny_socket_threads_to_front[fds[0]] = malloc(sizeof(pthread_t));
	preeny_socket_threads_to_back[fds[0]] = malloc(sizeof(pthread_t));

	if (selected_port!=-1)
	{
		// If a port is defined, call to original socket function and keep value in preeny_original_socket_to_front
		int orig_fd =  original_socket(domain, type, protocol);
		preeny_original_socket_to_front[fds[0]] = orig_fd;
		preeny_debug("Backup original socket for port selection feature\n");
	}

	r = pthread_create(preeny_socket_threads_to_front[fds[0]], NULL, (void*(*)(void*))preeny_socket_sync_to_front, (void *)front_socket);
	if (r)
	{
		perror("failed creating front-sync thread");
		return -1;
	}

	r = pthread_create(preeny_socket_threads_to_back[fds[0]], NULL, (void*(*)(void*))preeny_socket_sync_to_back, (void *)front_socket);
	if (r)
	{
		perror("failed creating back-sync thread");
		return -1;
	}

	return fds[0];
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	if (preeny_socket_threads_to_front[sockfd])
	{
		if (selected_port != -1 && sockfd != selected_fd) // Filtering port enabled
		{
				int orig_fd = preeny_original_socket_to_front[sockfd];
				return original_accept(orig_fd, addr, addrlen);
		}
		else
		{
			return dup(sockfd);
		}
	}
	else{
		return original_accept(sockfd, addr, addrlen);
	}
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
       accept(sockfd, addr, addrlen);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	if (preeny_socket_threads_to_front[sockfd])
	{
		int port = ntohs(((struct sockaddr_in*)addr)->sin_port);

		if (selected_port != -1)
		{
			if (selected_port != port)
			{
				preeny_info("Ignoring unselected socket on port %d (fd : %d)\n", port, sockfd);
				int orig_fd = preeny_original_socket_to_front[sockfd];
				return original_bind(orig_fd, addr, addrlen);
			}
			else
			{
				preeny_info("Filter enabled, only intercept call for sockfd=%d bind to %d port\n", sockfd, selected_port);
				pthread_mutex_lock(&lock);
				selected_fd = sockfd;
				pthread_mutex_unlock(&lock);

				return 0;
			}
		}
		else
		{
			preeny_info("Emulating bind on port %d\n", port);
			return 0;
		}
	}
	else
	{
		return original_bind(sockfd, addr, addrlen);
	}
}

int listen(int sockfd, int backlog)
{
	if (preeny_socket_threads_to_front[sockfd])
	{
		if (selected_port != -1 && sockfd != selected_fd){ // Filtering port enabled
				int orig_fd = preeny_original_socket_to_front[sockfd];
				return original_listen(orig_fd, backlog);
		}
		else{
			return 0;
		}
	}
	else{
		return original_listen(sockfd, backlog);
	}
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	if (preeny_socket_threads_to_front[sockfd])
	{
		if (selected_port != -1 && sockfd != selected_fd){ // Filtering enabled
				int orig_fd = preeny_original_socket_to_front[sockfd];
				return original_connect(orig_fd, addr, addrlen);
		}
		else
		{
			return 0;
		}
	}
	else
	{
		return original_connect(sockfd, addr, addrlen);
	}
}

int close(int fd)
{

	if (selected_port!=-1){
		int orig_fd = preeny_original_socket_to_front[fd];
		if (orig_fd > 0)
		{
			original_close(orig_fd);
		}
	}
	return original_close(fd);
}
