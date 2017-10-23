#include "Socket.hpp"

#include <atomic>
#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

#include "queue/readerwriterqueue.h"

using moodycamel::ReaderWriterQueue;

#ifdef DEBUG
	#define DEBUG_PRINT(x) std::cout << std::this_thread::get_id() << ":" << __FILE__ << ":" << __LINE__ << ": " << x << std::endl
	#define IF_DEBUG(x) x
#else
	#define DEBUG_PRINT(x)
	#define IF_DEBUG(x)
#endif

int main(void) {
	DEBUG_PRINT("IN DEBUG MODE");

	int sockfd = Socket::initServer("3490");

	struct Client {
		uint8_t id;
		Socket sock;

		Client(uint8_t id, int fd) : id(id), sock(fd) {}
	};

	ReaderWriterQueue<int> newClients;

	std::thread acceptThread([&sockfd, &newClients]() {
		int accepted = 0;

		while (accepted < 3) {
			int fd = Socket::accept(sockfd);
			if (fd == -1) {
				continue;
			}
			newClients.enqueue(fd);

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
							client->sock.writeQueue.enqueue(SimpleMessage::pack(MessageType::STAGING_PLAYER_CONNECT, newId));
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
										IF_DEBUG(startingTimer = 4.5f);

										std::cout << "Client voted to start the game" << std::endl;

										// TODO: queue up message saying player voted to start the game
										// or do it now?
										for (auto& c : clients) {
											c->sock.writeQueue.enqueue(SimpleMessage::pack(MessageType::STAGING_VOTE_TO_START, client->id));
										}
									}

									break;
								}

								case MessageType::STAGING_VETO_START: {
									if (starting) {
										starting = false;

										std::cout << "Client vetoed the game start" << std::endl;

										// TODO: queue up message start vetod by x message
										// or do it now?
										for (auto& c : clients) {
											c->sock.writeQueue.enqueue(SimpleMessage::pack(MessageType::STAGING_VETO_START, client->id));
										}
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
					}

					if (starting) {
						startingTimer += dt;

						if (startingTimer > 5.0f) {
							std::cout << "Game starting. Leaving staging." << std::endl;
							for (auto& c : clients) {
								Packet* packet = new Packet();
								packet->payload.emplace_back(STAGING_START_GAME);
								packet->header = packet->payload.size();
								c->sock.writeQueue.enqueue(packet);
							}
							state = IN_GAME;
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
					}

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
					}

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