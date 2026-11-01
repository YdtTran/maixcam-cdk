/* Bounded UDP recovery-network probe; never opens media or Bluetooth devices. */
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
int main(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(39000)};
    struct timeval timeout = {.tv_sec = 12};
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        bind(fd, (struct sockaddr *)&address, sizeof(address))) { perror("bind"); return 1; }
    char data[256]; struct sockaddr_in peer; socklen_t length = sizeof(peer);
    ssize_t count = recvfrom(fd, data, sizeof(data), 0, (struct sockaddr *)&peer, &length);
    if (count < 0) { perror("recvfrom"); close(fd); return 2; }
    printf("Received %zd bytes from %s:%u\n", count, inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
    int result = sendto(fd, data, count, 0, (struct sockaddr *)&peer, length) == count ? 0 : 3;
    close(fd); return result;
}
