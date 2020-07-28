#include <cassert>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "testsuite.hpp"

#define NAMED_PATH "/tmp/sockname"
#define ABSTRACT_PATH "\0/tmp/sockname"

DEFINE_TEST(unix_getname, ([] {
	int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(server_fd == -1)
		assert(!"server socket() failed");

	struct sockaddr_un server_addr;
	memset(&server_addr, 0, sizeof(struct sockaddr_un));
	server_addr.sun_family = AF_UNIX;
	strncpy(server_addr.sun_path, NAMED_PATH, sizeof(server_addr.sun_path) - 1);

	if(bind(server_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un)))
		assert(!"bind() failed");
	if(listen(server_fd, 50))
		assert(!"listen() failed");

	pid_t child = fork();
	if(!child) {
		int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if(client_fd == -1)
			assert(!"socket() client failed");
		if(connect(client_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un)))
			assert(!"connect() to server failed");
		sleep(2);
		exit(0);
	} else {
		sleep(1);
		int peer_fd = accept(server_fd, nullptr, nullptr);
		if(peer_fd == -1)
			assert(!"accept() failed");

		struct sockaddr_un peer_addr;
		socklen_t peer_length = sizeof(struct sockaddr_un);
		if(getsockname(peer_fd, (struct sockaddr *)&peer_addr, &peer_length))
			assert(!"getsockname() failed");
		assert(!strcmp(peer_addr.sun_path, NAMED_PATH));
		assert(peer_length == 17);
		memset(&peer_addr, 0, sizeof(struct sockaddr));
		peer_length = sizeof(struct sockaddr_un);
		if(getpeername(peer_fd, (struct sockaddr *)&peer_addr, &peer_length))
			assert(!"getpeername() failed");;
		assert(peer_addr.sun_path[0] == '\0');
		assert(peer_length == 5);
	}
	unlink(NAMED_PATH);
}))

DEFINE_TEST(unix_abstract_getname, ([] {
	int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(server_fd == -1)
		assert(!"server socket() failed");

	struct sockaddr_un server_addr;
	memset(&server_addr, 0, sizeof(struct sockaddr_un));
	server_addr.sun_family = AF_UNIX;
	strncpy(server_addr.sun_path, ABSTRACT_PATH, sizeof(server_addr.sun_path) - 1);

	if(bind(server_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un)))
		assert(!"bind() failed");
	if(listen(server_fd, 50))
		assert(!"listen() failed");

	pid_t child = fork();
	if(!child) {
		int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if(client_fd == -1)
			assert(!"socket() client failed");
		if(connect(client_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un)))
			assert(!"connect() to server failed");
		sleep(2);
		exit(0);
	} else {
		sleep(1);
		int peer_fd = accept(server_fd, nullptr, nullptr);
		if(peer_fd == -1)
			assert(!"accept() failed");

		struct sockaddr_un peer_addr;
		socklen_t peer_length = sizeof(struct sockaddr_un);
		if(getsockname(peer_fd, (struct sockaddr *)&peer_addr, &peer_length))
			assert(!"getsockname() failed");
		assert(!strcmp(peer_addr.sun_path, ABSTRACT_PATH));
		assert(peer_length == 17);
		memset(&peer_addr, 0, sizeof(struct sockaddr));
		peer_length = sizeof(struct sockaddr_un);
		if(getpeername(peer_fd, (struct sockaddr *)&peer_addr, &peer_length))
			assert(!"getpeername() failed");;
		assert(peer_addr.sun_path[0] == '\0');
		assert(peer_length == 5);
	}
	unlink(ABSTRACT_PATH);
}))

DEFINE_TEST(unix_unnamed_getname, ([] {
	int fds[2];
	int result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	if(result == -1)
		assert(!"server socketpair() failed");

	struct sockaddr_un server_addr;
	memset(&server_addr, 0, sizeof(struct sockaddr_un));
	server_addr.sun_family = AF_UNIX;
	//strncpy(server_addr.sun_path, SOCK1_PATH, sizeof(server_addr.sun_path) - 1);

	/*if(bind(server_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un)))
		assert(!"bind() failed");*/
	if(listen(fds[0], 50))
		assert(!"listen() failed");

	pid_t child = fork();
	if(!child) {
		/*int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if(client_fd == -1)
			assert(!"socket() client failed");*/
		if(connect(fds[1], (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un)))
			assert(!"connect() to server failed");
		sleep(2);
		exit(0);
	} else {
		sleep(1);
		int peer_fd = accept(fds[0], nullptr, nullptr);
		if(peer_fd == -1)
			assert(!"accept() failed");

		struct sockaddr_un peer_addr;
		socklen_t peer_length = sizeof(struct sockaddr_un);
		if(getsockname(peer_fd, (struct sockaddr *)&peer_addr, &peer_length))
			assert(!"getsockname() failed");
		//assert(peer_addr.sun_path[0] == '\0');
		assert(peer_length == sizeof(sa_family_t));
		memset(&peer_addr, 0, sizeof(struct sockaddr));
		peer_length = sizeof(struct sockaddr_un);
		if(getpeername(peer_fd, (struct sockaddr *)&peer_addr, &peer_length))
			assert(!"getpeername() failed");;
		//assert(peer_addr.sun_path[0] == '\0');
		assert(peer_length == sizeof(sa_family_t));
	}
	//unlink(SOCK1_PATH);
}))