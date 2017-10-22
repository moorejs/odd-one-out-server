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

#include "Socket.hpp"

#include <atomic>
#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

#include "queue/readerwriterqueue.h"

using namespace moodycamel;

#define PORT "3490"	// the port users will be connecting to

#define BACKLOG 10	// how many pending connections queue will hold

void* get_in_addr(struct sockaddr* sa) {
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

// probably could reuse the code above somehow
int get_in_port(struct sockaddr* sa) {
	if (sa->sa_family == AF_INET) {
		return ntohs(((struct sockaddr_in*)sa)->sin_port);
	}

	return ntohs(((struct sockaddr_in6*)sa)->sin6_port);
}

int main(void) {
	int sockfd, new_fd;	// listen on sock_fd, new connection on new_fd
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr;	// connector's address information
	socklen_t sin_size;
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

	struct Client {
		uint8_t id;
		Socket sock;

		Client(uint8_t id, int fd) : id(id), sock(fd) {}
	};

	ReaderWriterQueue<int> newClients;
	std::thread acceptThread([&]() {
		int accepted = 0;
		while (accepted < 3) {
			sin_size = sizeof their_addr;
			new_fd = accept(sockfd, (struct sockaddr*)&their_addr, &sin_size);
			if (new_fd == -1) {
				perror("accept");
				continue;
			}

			// convert IP to string to print
			inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr*)&their_addr), s, sizeof s);
			printf("server: got connection from %s %d\n", s, get_in_port((struct sockaddr*)&their_addr));

			newClients.enqueue(new_fd);

			accepted++;
		}
		std::cout << "Done accepting clients" << std::endl;
		close(sockfd);
	});
	acceptThread.detach();

	// ------- game state --------
	std::vector<std::unique_ptr<Client>> clients;

	enum State {
		STAGING,
		IN_GAME,
	} state = STAGING;

	// staging game state
	bool starting = false;
	float startingTimer = 0.0f;

	// clock timing logic from https://stackoverflow.com/a/20381816
	typedef std::chrono::duration<int, std::ratio<1, 10>> frame_duration;
	auto delta = frame_duration(1);
	float dt = 1.0f / 10.0f;


	std::thread gameLoop([&]() {
		while (true) {
			auto start_time = std::chrono::steady_clock::now();

			switch (state) {
				case STAGING: {

					// process newly accepted connections
					int fd;
					while (newClients.try_dequeue(fd)) {
						uint8_t newId = clients.size();

						// tell other clients
						for (auto& client : clients) {
							//Packet* packet = new Packet();
							/*
							packet->payload.push_back(STAGING_PLAYER_CONNECT);
							packet->payload.push_back(newId);

							packet->header = packet->payload.size();*/

							client->sock.writeQueue.enqueue(ConnectMessage::pack(newId));
						};

						clients.emplace_back(new Client(newId, fd));
					}

					for (auto& client : clients) {
						// read pending messages from clients
						Packet* out;
						while (client->sock.readQueue.try_dequeue(out)) {
							if (!out) {
								std::cout << "Bad packet from client" << std::endl;
								continue;
							}

							switch (out->payload.at(0)) { // message type
								case MessageType::STAGING_VOTE_TO_START: {
									if (!starting) {
										starting = true;
										startingTimer = 0.0f;

										std::cout << "Client voted to start the game" << std::endl;

										// TODO: queue up message saying player voted to start the game
										// or do it now?
										for (auto& c : clients) {
											c->sock.writeQueue.enqueue(VoteToStartMessage::pack(client->id));
										}
									}

									break;
								}

								case MessageType::STAGING_VETO_START: {
									if (starting) {
										starting = false;

										std::cout << "Client vetoed the game start" << std::endl;

										// TODO: queue up message start vetod by x message
									}

									break;
								}

								default: {
									std::cout << "Unknown starting message type: " << out->payload.at(0) << std::endl;
									break;
								}
							}

							delete out;
						}
					};

					if (starting) {
						startingTimer += dt;

						if (startingTimer > 5.0f) {
							std::cout << "Game starting. Leaving staging (not really)." << std::endl;
							// TODO: send out start game message
						}
					}

					// write state updates

					break;
				}

				case IN_GAME: {
					for (auto& client : clients) {
						// read pending messages from clients
						Packet* out;
						while (client->sock.readQueue.try_dequeue(out)) {
							if (!out) {
								std::cout << "Bad packet from client" << std::endl;
								continue;
							}

							switch (out->payload.at(0)) { // message type

								default: {
									std::cout << "Unknown game message type: " << out->payload.at(0) << std::endl;
									break;
								}
							}

							delete out;
						}
					};

					// write state updates
					for (auto& client : clients) {
						Packet* delta = new Packet();
						delta->payload.push_back('H');
						delta->payload.push_back('E');
						delta->payload.push_back('L');
						delta->payload.push_back('L');
						delta->payload.push_back('O');
						delta->header = delta->payload.size();
						client->sock.writeQueue.enqueue(delta);
					};

					break;
				}
			}

			// sleep if necessary
			std::this_thread::sleep_until(start_time + delta);
		}
	});

	gameLoop.join();

	return 0;
}