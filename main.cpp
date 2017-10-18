// Much of the server code here is from
// http://beej.us/guide/bgnet/output/html/multipage/clientserver.html#simpleserver
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <atomic>
#include <array>
#include <iostream>
#include <thread>
#include <vector>

#include "queue/readerwriterqueue.h"

using namespace moodycamel;

#define PORT "3490"	// the port users will be connecting to

#define BACKLOG 10	// how many pending connections queue will hold

// get sockaddr, IPv4 or IPv6:
void* get_in_addr(struct sockaddr* sa) {
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(void) {
	int sockfd, new_fd;	// listen on sock_fd, new connection on new_fd
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr;	// connector's address information
	socklen_t sin_size;
	struct sigaction sa;
	int yes = 1;
	char s[INET6_ADDRSTRLEN];
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;	// use my IP

	if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and bind to the first we can
	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
			perror("server: socket");
			continue;
		}

		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
			perror("setsockopt");
			exit(1);
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("server: bind");
			continue;
		}

		break;
	}

	freeaddrinfo(servinfo);	// all done with this structure

	if (p == NULL) {
		fprintf(stderr, "server: failed to bind\n");
		exit(1);
	}

	if (listen(sockfd, BACKLOG) == -1) {
		perror("listen");
		exit(1);
	}

	printf("server: waiting for connections...\n");

	// TODO: figure out how we want to format incoming messages
	struct Message {
		uint8_t header;
		std::vector<uint8_t> payload;
	};

	struct Socket {
		int fd;
		std::atomic<bool> connected;

		Socket(int fd) : fd(fd), connected(true) {}
		Socket(const Socket& other) = delete;
		Socket(Socket&& other) : fd(other.fd), connected(other.connected.load()) {}

		ReaderWriterQueue<Message*> readQueue;
		ReaderWriterQueue<Message*> writeQueue;

		int recv(void* buffer, size_t size) {
			// TODO: error handling
			int n = ::recv(fd, buffer, size, 0);

			if (n == 0) {
				connected = false;
			}

			return n;
		}
		int send(const std::vector<char>& data) {
			// TODO: error handling

			// TODO: send all functionality to ensure everything is send
			return ::send(fd, &data, data.size(), 0); }
	};

	struct Client {
		Socket sock;

		Client(int fd) : sock(fd) {}
		Client(const Client& other) = delete;
		Client(Client&& other) : sock(std::move(other.sock)) {}
	};
	std::vector<Client> clients;
	int accepted = 0;

	while (accepted < 1) {	// main accept() loop
		sin_size = sizeof their_addr;
		new_fd = accept(sockfd, (struct sockaddr*)&their_addr, &sin_size);
		if (new_fd == -1) {
			perror("accept");
			continue;
		}

		// convert IP to string to print
		inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr*)&their_addr), s, sizeof s);
		printf("server: got connection from %s\n", s);

		char buffer[1024] = "Hello World";
		if (send(new_fd, buffer, 1024, 0) == -1) {
			perror("send");
		}

		clients.emplace_back(new_fd);

		accepted += 1;
	}
	close(sockfd);

	for (auto& client : clients) {
		std::thread test([&]() {
			while (client.sock.connected) {
				Message* message = new Message();
				client.sock.recv(&message->header, sizeof message->header);

				message->payload.reserve(message->header);
				client.sock.recv(&message->payload, message->header);

				client.sock.readQueue.enqueue(message);
			}
		});
		test.detach();
	}

	// clock timing logic from https://stackoverflow.com/a/20381816
	typedef std::chrono::duration<int, std::ratio<1, 20>> frame_duration;
	auto delta = frame_duration(1);
	float dt = 1.0f / 60.0f;
	std::thread t([&]() {
		while (true) {
			auto start_time = std::chrono::steady_clock::now();
			auto end_time = start_time + delta;

			for (auto& client : clients) {
				// TODO: check for disconnections

				// read messages off queue
				Message* out;
				while (client.sock.readQueue.try_dequeue(out)) {
					std::cout << "server read " << out->header << "from a client" << std::endl;
				}
			}

			// Sleep if necessary
			std::this_thread::sleep_until(end_time);
		}
	});

	t.join();	// main thread waits for t to finish

	return 0;
}