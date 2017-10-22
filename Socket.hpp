#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include <iostream>

// Some of these may be superfluous
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "queue/readerwriterqueue.h"

using moodycamel::ReaderWriterQueue;
using moodycamel::BlockingReaderWriterQueue;

struct Packet {
  uint8_t header;
  std::vector<uint8_t> payload;
};

enum MessageType {
  STAGING_PLAYER_CONNECT,
  STAGING_PLAYER_DISCONNECT,
  STAGING_VOTE_TO_START,
  STAGING_VETO_START,
  STAGING_START_GAME,
  INPUT,
};

// simple message just has type and id of client
struct SimpleMessage {
  MessageType type : 8;
  uint8_t id;

  static Packet* pack(MessageType type, uint8_t id = 255) {
    Packet* packet = new Packet();

    packet->payload.emplace_back(type);
    if (id != 255) {
      packet->payload.emplace_back(id);
    }

    packet->header = packet->payload.size();

    return packet;
  }

  static const SimpleMessage* unpack(const std::vector<uint8_t>& payload) {
    return reinterpret_cast<const SimpleMessage*>(payload.data());
  }
};

class Socket {

  int fd;
  std::atomic<bool> connected;

public:
  Socket(int fd)
    : fd(fd),
      connected(true),
      readThread([&]() {
        while (true) {
          if (!connected) {
            std::cout << "Disconnected, exiting read thread" << std::endl;
            return;
          }

          readQueue.enqueue(getPacket());
        }
      }),
      writeThread([&]() {
        while (true) {
          Packet* packet;
          writeQueue.wait_dequeue(packet);

          if (!packet) {
            continue;
          }

          if (!connected) {
            delete packet;
            std::cout << "Disconnected, not writing packet and exiting write thread" << std::endl;
            return;
          }

          sendPacket(packet);
        }
      })
  {
    readThread.detach();
    writeThread.detach();
  }

  // no move or copying
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) = delete;
  Socket& operator=(Socket&&) = delete;

  void close() {
    ::close(fd);
  }

  bool isConnected() {
    return connected;
  }

  ReaderWriterQueue<Packet*> readQueue;
  BlockingReaderWriterQueue<Packet*> writeQueue;

  std::thread readThread;
  std::thread writeThread;

private:
  // recv that does error checking and connection checking
  int recv(void* buffer, size_t size) {
    int n = ::recv(fd, buffer, size, 0);

    if (n < 0) {
      perror("recv");
    }

    if (n == 0) {
      connected = false;
    }

    return n;
  }

  // send that does error checking and connection checking
  int send(void* buffer, size_t size) {
    int n = ::send(fd, buffer, size, 0);

    if (n < 0) {
      perror("send");
    }

    if (n == 0) {
      connected = false;
    }

    return n;
  }

  // get complete packet
  Packet* getPacket() {
    int so_far;
    Packet* packet = new Packet();

    // get header (could be multiple bytes in future)
    so_far = 0;
    do {
      int n = recv(&packet->header + so_far, (sizeof packet->header) - so_far);
      so_far += n;

      if (n == 0) {
        delete packet;
        return nullptr;
      }
    } while (so_far < sizeof packet->header);

    assert(packet->header > 0);

    // get payload
    packet->payload.resize(packet->header);
    so_far = 0;
    do {
      int n = recv(packet->payload.data() + so_far, packet->header - so_far);
      so_far += n;

      if (n == 0) {
        delete packet;
        return nullptr;
      }
    } while (so_far < packet->header);
    packet->payload.resize(packet->header);

    return packet;
  }

  // sends all data in vector
  bool sendPacket(Packet* packet) {
    // send header (could be multiple bytes in future)
    int n = send(&packet->header, 1);
    if (n <= 0) {
      return false;
    }

    int so_far = 0;
    do {
      int m = send(packet->payload.data() + so_far, packet->payload.size() - so_far);
      so_far += m;
      if (m <= 0) {
        return false;
      }
    } while (so_far < packet->payload.size());

    return true;
  }
};