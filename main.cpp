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

		enum Role { // TODO: reuse code from client
			NONE,
			ROBBER,
			COP,
		} role = Role::NONE;

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

	struct StagingState {
		bool starting = false;
		float startingTimer = 0.0f;
		Client* robber = nullptr; // everyone else assumed to be cop
		unsigned playerUnready = 0;
	} stagingState;

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

						std::vector<uint8_t> syncData;
						syncData.push_back(newId);
						for (auto& client : clients) {
							client->sock.writeQueue.enqueue(Packet::pack(MessageType::STAGING_PLAYER_CONNECT, {newId}));
							syncData.push_back(client->id);
							syncData.push_back(client->role);
						};

						clients.emplace_back(new Client(newId, fd));

						clients.back()->sock.writeQueue.enqueue(Packet::pack(MessageType::STAGING_PLAYER_SYNC, syncData));

						stagingState.playerUnready += 1;

						// TODO: tell new client about game settings / staging state
					}

					for (auto& client : clients) {
						if (!client->sock.isConnected()) {
							continue;
						}

						// read pending messages from clients
						Packet* out;
						while (client->sock.readQueue.try_dequeue(out)) {
							if (!out) {
								std::cout << "Bad packet from client" << std::endl;
								continue;
							}

							switch (out->payload.at(0)) { // message type
								case MessageType::STAGING_VOTE_TO_START: {
									if (stagingState.starting) {
										break;
									}

									if (clients.size() < 2) {
										break;
									}

									if (stagingState.playerUnready > 0) {
										// TODO: error message saying not all players are ready
										break;
									}

									stagingState.starting = true;
									stagingState.startingTimer = 0.0f;
									IF_DEBUG(stagingState.startingTimer = 3.0f);

									std::cout << "Client voted to start the game" << std::endl;

									// TODO: queue up message saying player voted to start the game
									// or do it now?
									for (auto& c : clients) {
										c->sock.writeQueue.enqueue(Packet::pack(MessageType::STAGING_VOTE_TO_START, {client->id}));
									}

									break;
								}

								case MessageType::STAGING_VETO_START: {
									if (!stagingState.starting) {
										break;
									}

									stagingState.starting = false;

									std::cout << "Client vetoed the game start" << std::endl;

									// TODO: queue up message start vetod by x message
									// or do it now?
									for (auto& c : clients) {
										c->sock.writeQueue.enqueue(Packet::pack(MessageType::STAGING_VETO_START, {client->id}));
									}

									break;
								}

								case MessageType::STAGING_ROLE_CHANGE: {
									if (stagingState.starting) {
										break;
									}

									DEBUG_PRINT("client " << (int)client->id << " wants role " << int(out->payload[1]));

									if (out->payload[1] == Client::Role::ROBBER && stagingState.robber) { // can't be robber if someone else is
										client->sock.writeQueue.enqueue(Packet::pack(MessageType::STAGING_ROLE_CHANGE_REJECTION, {stagingState.robber->id}));
										break;
									}

									if (client->role == Client::Role::NONE) { // client has never selected anything
										stagingState.playerUnready -= 1;
									}

									// client is no longer robber
									if (client->role == Client::Role::ROBBER && out->payload[1] != Client::Role::ROBBER) {
										stagingState.robber = nullptr;
									}

									if (out->payload[1] == Client::Role::ROBBER) { // desires to be robber
										stagingState.robber = client.get();
									}

									client->role = static_cast<Client::Role>(out->payload[1]);

									// tell players of role change
									for (auto& c : clients) {
										c->sock.writeQueue.enqueue(Packet::pack(MessageType::STAGING_ROLE_CHANGE, {client->id, out->payload[1]}));
									}

									break;
								}

								default: {
									std::cout << "Unknown starting message type: " << (int)out->payload.at(0) << std::endl;
									break;
								}
							}

							delete out;
						}
					}

					if (stagingState.starting) {
						stagingState.startingTimer += dt;

						if (stagingState.startingTimer > 5.0f) {
							std::cout << "Game starting. Leaving staging." << std::endl;
							for (auto& c : clients) {
								c->sock.writeQueue.enqueue(Packet::pack(MessageType::STAGING_START_GAME, { 200 }));
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
									std::cout << "Unknown game message type: " << (int)out->payload[0] << std::endl;
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