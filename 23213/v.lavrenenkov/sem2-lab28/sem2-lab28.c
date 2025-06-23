#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/select.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <termios.h>

#define SCREEN_HEIGHT 25
#define BUFFER_SIZE (1<<20)

typedef struct {
    char data[BUFFER_SIZE];
    int head;       
    int tail;       
    int count;      
} RingBuffer;
void rb_init(RingBuffer *rb) {
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}
bool rb_is_full(RingBuffer *rb) {
    return rb->count == BUFFER_SIZE;
}
bool rb_is_empty(RingBuffer *rb) {
    return rb->count == 0;
}
size_t rb_enqueue(RingBuffer *rb, const char *src, size_t len) {
    size_t written = 0;
    while (written < len && rb->count < BUFFER_SIZE) {
        rb->data[rb->head] = src[written];
        rb->head = (rb->head + 1) % BUFFER_SIZE;
        rb->count++;
        written++;
    }
    return written;
}
size_t rb_dequeue(RingBuffer *rb, char *dst, size_t len) {
    size_t read = 0;
    while (read < len && rb->count > 0) {
        dst[read] = rb->data[rb->tail];
        rb->tail = (rb->tail + 1) % BUFFER_SIZE;
        rb->count--;
        read++;
    }
    return read;
}

size_t rd_enqueue_tail(RingBuffer *rb, const char *src, size_t len) {
    size_t written = 0;
    for (size_t i = len; i > 0 && rb->count < BUFFER_SIZE; i--) {
        rb->tail = (rb->tail - 1 + BUFFER_SIZE) % BUFFER_SIZE;
        rb->data[rb->tail] = src[i - 1];
        rb->count++;
        written++;
    }
    return written;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <URL>\n", argv[0]);
        return 1;
    }
    char *url = argv[1];
    if (strncmp(url, "http://", 7) != 0) {
        fprintf(stderr, "Only http:// URLs supported\n");
        return 1;
    }
    char *host_start = url + 7;
    char *host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/') {
        host_end++;
    }
    char *host = malloc(host_end - host_start + 1);
    if (!host) {
        perror("malloc");
        return 1;
    }
    strncpy(host, host_start, host_end - host_start);
    host[host_end - host_start] = '\0';
    int port = 80;
    char *path = "/";
    if (*host_end == ':') {
        char *port_start = host_end + 1;
        char *port_end = port_start;
        while (*port_end && *port_end != '/') {
            port_end++;
        }
        if (port_end > port_start) {
            char port_str[16];
            strncpy(port_str, port_start, port_end - port_start);
            port_str[port_end - port_start] = '\0';
            port = atoi(port_str);
        }
        if (*port_end == '/') {
            path = port_end;
        }
    } else if (*host_end == '/') {
        path = host_end;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        free(host);
        return 1;
    }
    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "Could not resolve host: %s\n", host);
        close(sock);
        free(host);
        return 1;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = *((struct in_addr*)he->h_addr)
    };
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr))) {
        perror("connect");
        close(sock);
        free(host);
        return 1;
    }
    char request[1024];
    sprintf(request, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    if (send(sock, request, strlen(request), 0) < 0) {
        perror("send");
        close(sock);
        free(host);
        return 1;
    }

    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN] = 1;  
    newt.c_cc[VTIME] = 0; 
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    fd_set read_fds;
    fd_set write_fds;
    int line_count = 0;
    int in_body = 0;
    int paused = 0;
    int end = 0;
    char bufferRecv[BUFSIZ];
    char bufferWrite[BUFSIZ];
    RingBuffer bufferRb;
    rb_init(&bufferRb);
    while (end == 0 || rb_is_empty(&bufferRb) == false) {
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        if(!end) FD_SET(sock, &read_fds);
        if(rb_is_empty(&bufferRb) == false && !paused) FD_SET(STDOUT_FILENO, &write_fds);
        if(paused) FD_SET(STDIN_FILENO, &read_fds);
        select(sock + 1, &read_fds, &write_fds, NULL, NULL); 
        if(FD_ISSET(sock, &read_fds)) {
            ssize_t bytes_read = recv(sock, bufferRecv, BUFSIZ - 1, 0);
            if (bytes_read == 0) end = 1;
            if (!in_body) {
                char *body_start = strstr(bufferRecv, "\r\n\r\n");
                if (body_start) {
                    in_body = 1;
                    char *body = body_start + 4;
                    bytes_read -= (body - bufferRecv);
                    memmove(bufferRecv, body, bytes_read + 1);
                } else {
                    continue;
                }
            }
            rb_enqueue(&bufferRb, bufferRecv, bytes_read);
        }
        if(FD_ISSET(STDOUT_FILENO, &write_fds)) {
            size_t readed = rb_dequeue(&bufferRb, bufferWrite, BUFSIZ);
            size_t last = 0;
            size_t i;
            int bigstr = 1;
            for(i = 0; i < readed; i++) {
                if(bufferWrite[i] == '\n') {
                    bigstr = 0;
                    line_count++;
                    write(STDOUT_FILENO, bufferWrite + last, i - last + 1);
                    last = i + 1;
                }
                if(line_count == SCREEN_HEIGHT) {
                    if(last < readed) {  
                        rd_enqueue_tail(&bufferRb, bufferWrite + last, readed - last);
                    }
                    paused = 1;
                    printf("\nPress space to scroll down...\n\n");
                    break;
                }
            }
            if(bigstr) {
                write(STDOUT_FILENO, bufferWrite, readed);
            }
            if(!paused && last < readed && !bigstr) {
                rd_enqueue_tail(&bufferRb, bufferWrite + last, readed - last);
            }
        }
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char key;
            if (read(STDIN_FILENO, &key, 1) == 1 && key == ' ') {
                paused = 0;
                line_count = 0;
            }
        }
    }
    putchar('\n');
    close(sock);
    free(host);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return 0;
}
