#include <domain_sockets.h>
#include <ns_limits.h>
#include <poll_helpers.h>

#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <assert.h>
#include <poll.h>

#define MAX_BUF_SZ 1024

/* Whatever information you need to save for a server (name, pipes, pid, etc...) */
struct server_info {
	/* Add your server information here! */
	char * name;
	char * bin; // bin = "./yell;"
	int domain;
	int read[2];
	int write[2];
	int pid;
	int fd;
	int accept_fd;
	int read_fd;
	int write_fd;
	/*
		1. the server's name,
		2. the server's binary file,
		3. the server's domain socket descriptor on which we `accept` new clients,
		4. the server's `pipe` descriptors we can use to communicate with it, and
		5. the server's pid.
	*/
};

/* All of our servers */
struct server_info servers[MAX_SRV];

/* Each file descriptor can be used to lookup the server it is associated with here */
struct server_info *client_fds[MAX_FDS];

/* The array of pollfds we want events for; passed to poll */
struct pollfd poll_fds[MAX_FDS];
int num_fds = 0;

/*
 * If you want to use these functions to add and remove descriptors
 * from the `poll_fds`, feel free! If you'd prefer to use the logic
 * from the lab, feel free!
 */
void
poll_create_fd(int fd)
{
	assert(poll_fds[num_fds].fd == 0);
	poll_fds[num_fds] = (struct pollfd) {
		.fd     = fd,
		.events = POLLIN
	};
	num_fds++;
}

void
poll_remove_fd(int fd)
{
	int i;
	struct pollfd *pfd = NULL;

	assert(fd != 0);
	for (i = 0; i < MAX_FDS; i++) {
		if (fd == poll_fds[i].fd) {
			pfd = &poll_fds[i];
			break;
		}
	}
	assert(pfd != NULL);

	close(fd);
	/* replace the fd by coping in the last one to fill the gap */
	*pfd = poll_fds[num_fds - 1];
	client_fds[i] = client_fds[num_fds - 1]; // update the corresponding client_fds entry
	poll_fds[num_fds - 1].fd = 0;
	client_fds[num_fds - 1] = NULL; // set the last client_fds entry to NULL
	num_fds--;
}


int new_domain_socket_server_create(char *name) {
    int sockfd;
    struct sockaddr_un addr;

    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, name, sizeof(addr.sun_path) - 1);

    // Remove any existing socket file with the same name
    unlink(addr.sun_path);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        return -1;
    }

    if (listen(sockfd, 5) == -1) {
        return -1;
    }

    return sockfd;
}

int
server_create(char *name, char *binary)
{
	struct server_info *srv = NULL;
	int i;

	// Find empty server
	for (i = 0; i < MAX_SRV; i++) {
		if (servers[i].name == NULL) {
			srv = &servers[i];
			break;
		}
	}

	if (srv == NULL) {
		return -1;
	}

	// Setup read and write pipes
	int read_pipe[2], write_pipe[2];
	if (pipe(read_pipe) < 0 || pipe(write_pipe) < 0) {
		return -1;
	}

	// Execute 
	int pid = fork();
	if (pid < 0) {
		return -1;
	}

	if (pid == 0) { // Child process
		close(read_pipe[0]);
		close(write_pipe[1]);

		dup2(write_pipe[0], STDIN_FILENO);
		dup2(read_pipe[1], STDOUT_FILENO);

		// Close unused file descriptors after duplicating
		close(write_pipe[0]);
		close(read_pipe[1]);

		char *args[] = {binary, NULL};
		execvp(binary, args);
		exit(EXIT_FAILURE);
	}

	close(read_pipe[1]);
	close(write_pipe[0]);

	// Store server information
	srv->name = strdup(name);
	srv->bin = strdup(binary);
	srv->pid = pid;
	srv->read_fd = read_pipe[0];
	srv->write_fd = write_pipe[1];

    // Create a domain socket
    int sockfd = new_domain_socket_server_create(name);
    if (sockfd < 0) {
        return -1;
    }
    srv->accept_fd = sockfd;

    poll_create_fd(sockfd);
    client_fds[num_fds - 1] = srv;

    return 0;
}



void initialize_fds(){
	int c = 0;
	for (c = 0; c < MAX_FDS; c++) {
		poll_fds[c].fd = 0;
	}
}

int
main(int argc, char *argv[])
{
	ignore_sigpipe();

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <name_server_map_file>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	initialize_fds();
	int i;
	int new_client;
	int amnt;
	// char chunkachunk[MAX_BUF_SZ];
	// memset(chunkachunk, 0, MAX_BUF_SZ);

	if (read_server_map(argv[1], server_create)) panic("Configuration file error");

	/*** The event loop ***/
	while (1) {
		int ret;

		/*** Poll; wait for client/server activity, no timeout ***/
		ret = poll(poll_fds, num_fds, -1);
		if (ret <= 0) panic("poll error");

		/*** Accept file descriptors for different servers have new clients connecting! ***/
		for (i = 0; i < num_fds; i++) {
			if (poll_fds[i].revents & POLLIN) {
				if(client_fds[i] != NULL && client_fds[i]->accept_fd == poll_fds[i].fd){
					if ((new_client = accept(poll_fds[i].fd, NULL, NULL)) == -1) panic("server accept");
					
					else{
						poll_create_fd(new_client);
						client_fds[num_fds - 1] = client_fds[i];
						poll_fds[i].revents = 0;
					}
				}
			}
		}

		/* ... */


		/*** Communicate with clients! ***/

		for (i = 1; i < num_fds; i++) {
			if (poll_fds[i].revents & (POLLHUP | POLLERR)) {
				poll_remove_fd(poll_fds[i].fd);
				client_fds[i] = NULL;
				poll_fds[i].revents = 0;
				i--;
			}
			if (poll_fds[i].revents & POLLIN) {

				char chunkachunk[MAX_BUF_SZ];
				struct server_info *srv = client_fds[i];
				amnt = read(poll_fds[i].fd, chunkachunk, sizeof(chunkachunk) - 1);

				// if (amnt == -1) panic("Error reading from client\n");
				// else if (amnt == 0) panic("zero read from client");

				if(amnt < 0){
					//Error read from client, continue
					panic("Error reading from client\n");
				}
				else {
					chunkachunk[amnt] = '\0';

					if(amnt != 0){

						if(chunkachunk != NULL){
							write(srv->write_fd, chunkachunk, amnt);
							amnt = read(srv->read_fd, chunkachunk, sizeof(chunkachunk) - 1);
							chunkachunk[amnt] = '\0';

							write(poll_fds[i].fd, chunkachunk, amnt);
						}
					}
				}
				poll_fds[i].revents = 0;
			} 
		}

		/* ... */
	}

	return 0;
}