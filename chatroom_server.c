
/*
 * CSCI 4220 - Assignment 2 Reference Solution
 * Concurrent Chatroom Server (select() + pthread worker pool)
 * Classic IRC-style "/me" action messages: *username text*
 *
 * This program demonstrates:
 *   - I/O multiplexing with select()
 *   - Multi-threaded worker pool using pthreads
 *   - Thread-safe producer/consumer queues
 *   - Message broadcasting to multiple clients
 *   - Basic command handling (/who, /me, /quit)
 *
 * Build:
 *   clang -Wall -Wextra -O2 -pthread chatroom_server.c -o chatroom_server.out
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_NAME     32
#define MAX_MSG      1024
#define MAX_CLIENTS  64
#define INBUF        2048
#define BACKLOG      0


/* ---------------- Data Structures ---------------- */
typedef struct Job {
    int sender_fd;                  // The file descriptor (socket) of the client who sent the message
    char *username;                 // Username of the sender
    char *msg;                      // Raw message text sent by the client
    struct Job *next;               // Pointer to the next Job in the queue (linked-list structure)
    struct Job *prev;               // doubly linked (makes q_pop more efficient)
    int broadcast;                  // 1 = send to everyone else, 0 = only send to sender_fd
    int quit;                       // 1 = disconnect client, 0 = keep client connected
} Job;

/*
 * Thread-safe FIFO queue structure.
 * Used for both job_queue (raw messages from clients)
 * and bcast_queue (formatted messages ready to broadcast).
 */
typedef struct Queue {
    Job *head;              // Pointer to the first Job in the queue
    Job *tail;              // Pointer to the last Job in the queue
    pthread_mutex_t mtx;    // Mutex to protect access to the queue
    pthread_cond_t cv;      // Condition variable for thread signaling
    int closed;             // Flag: 1 when queue is closed (no new Jobs)
} Queue;

typedef struct Client {
    int connfd;
    char username[MAX_NAME + 1];
} Client;


/* ---------------- Global Variables ---------------- */
volatile sig_atomic_t shutdownFlag = 0;
static Queue jobQueue, bcastQueue;
int listenfd;
int numWorkers;
int numClients;
Client *clients;
pthread_mutex_t clientsMutex;
pthread_t *workers;


/* ---------------- Queue Utilities ---------------- */
static void q_init(Queue *q) {
    q->head = NULL;
    q->tail = NULL;
    pthread_mutex_init(&(q->mtx), NULL);
    pthread_cond_init(&(q->cv), NULL);
    q->closed = 0;
}
static void q_close(Queue *q) {
    q->closed = 1;
}
static void q_push(Queue *q, Job *j) {
    j->prev = NULL;
    j->next = q->head;
    if (q->head == NULL) {
        q->head = j;
        q->tail = j;
    } else {
        q->head->prev = j;
        q->head = j;
    }
    pthread_cond_signal(&(q->cv));
}
static Job *q_pop(Queue *q) {
    if (q->tail == NULL) 
        return NULL;
    Job *j = q->tail;
    q->tail = j->prev;
    if (q->tail == NULL)
        q->head = NULL;
    return j;
}


/* ---------------- Server Setup Functions ---------------- */
void printUsage() {
    printf("USAGE: ./chatroom_server.out [port] [num_workers] [max_clients]\n");
    printf("\tport:         TCP port for server to listen on\n");
    printf("\tnum_workers:  Number of worker threads in processing pool (2-4)\n");
    printf("\tmax_clients:  Maximum number of simultaenous clients (<= %d)\n", MAX_CLIENTS);
}

int isInt(char *str) {
    for (unsigned long i = 0; i < strlen(str); i++) {
        if (!isdigit(str[i]))
            return 0;
    }
    return 1;
}

// -1 if invalid, port # if valid
int validPort(char *port) {
    if (!isInt(port))
        return -1;
    in_port_t p = atoi(port);
    if (p < 0)
        return -1;
    return p;
}

// -1 if invalid, # workers if valid
int validNumWorkers(char *numWorkers) {
    if (!isInt(numWorkers)) 
        return -1;
    int nw = atoi(numWorkers);
    if (nw < 2 || nw > 4)
        return -1;
    return nw;
}

// -1 if invalid, # max clients if valid
int validNumClients(char *numClients) {
    if (!isInt(numClients))
        return -1;
    int mc = atoi(numClients);
    if (mc < 1 || mc > MAX_CLIENTS)
        return -1;
    return mc;
}

// create listening socket
int listenSocket(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    if (errno != EINTR)
        perror("socket() failed");
    return -1;
  }
  
  struct sockaddr_in servAddr;
  memset(&servAddr, 0, sizeof(servAddr));
  servAddr.sin_family = AF_INET;
  servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servAddr.sin_port = htons(port);
  
  if (bind(fd, (struct sockaddr *)&servAddr, sizeof(servAddr)) == -1) {
    if (errno != EINTR)
        perror("bind() failed");
    close(fd);
    return -1;
  }
  if (listen(fd, BACKLOG) == -1) {
    if (errno != EINTR)
        perror("listen() failed");
    close(fd);
    return -1;
  }

  return fd;
}

void cleanup() {
    // close all sockets
    close(listenfd);
    for (int i = 0; i < numClients; i++) {
        if (clients[i].connfd != 0)
            close(clients[i].connfd);
    }
    free(clients);

    // free all jobs
    while (jobQueue.head != NULL) {
        Job *next = jobQueue.head->next;
        free(jobQueue.head->msg);
        free(jobQueue.head);
        jobQueue.head = next;
    }
    while (bcastQueue.head != NULL) {
        Job *next = bcastQueue.head->next;
        free(bcastQueue.head->msg);
        free(bcastQueue.head);
        bcastQueue.head = next;
    }
    
    // join worker threads
    pthread_mutex_lock(&jobQueue.mtx);
    q_close(&jobQueue);
    pthread_mutex_unlock(&jobQueue.mtx);
    pthread_mutex_lock(&bcastQueue.mtx);
    q_close(&bcastQueue);
    pthread_mutex_unlock(&bcastQueue.mtx);
    pthread_cond_broadcast(&jobQueue.cv); //wake up workers
    for (int i = 0; i < numWorkers; i++)
        pthread_join(workers[i], NULL);
    free(workers);
    
    // only destroy mutexes and conditional variables once all workers are done using them
    pthread_mutex_destroy(&clientsMutex);
    pthread_mutex_destroy(&jobQueue.mtx);
    pthread_mutex_destroy(&bcastQueue.mtx);
    pthread_cond_destroy(&jobQueue.cv);
    pthread_cond_destroy(&bcastQueue.cv);
}

void shutdownServer(__attribute__((unused)) int signum) {
    shutdownFlag = 1;
}


/* ---------------- Send / Receive Functions ---------------- */
int tcpSend(char *msg, int len, int fd) {
    int bytesSent = 0;
    while (bytesSent != len) {
        int n = send(fd, msg + bytesSent, len - bytesSent, 0);
        if (n == -1)
            return -1;
        bytesSent += n;
    }
    return bytesSent;
}

int tcpRecv(char *buf, int len, int fd) {
    int bytesRcvd = 0;
    while (bytesRcvd != len) {
        if (recv(fd, buf + bytesRcvd, 1, 0) <= 0) //read 1 byte at a time so multiple lines dont get read
            return -1;
        bytesRcvd++;
        if (buf[bytesRcvd-1] == '\n')
            break;
    }
    buf[bytesRcvd-1] = '\0';
    return bytesRcvd;
}


/* ---------------- Other Helpers ---------------- */
void strToLowerCase(char *str) {
    for (size_t i = 0; i < strlen(str); i++)
        str[i] = tolower(str[i]);
}
void trimStr(char *str) {
    for (int i = strlen(str)-1; i >= 0 && isspace(str[i]); i--)
        str[i] = '\0';
}

/* ---------------- Main Thread Functions ---------------- */
int validName(int ind, char *name) {
    strToLowerCase(name);
    for (size_t i = 0; i < strlen(name); i++) {
        if (!isalnum(name[i]) && name[i] != '_')
            return -2;
    }
    pthread_mutex_lock(&clientsMutex);
    for (int i = 0; i < numClients; i++) {
        if (i != ind && clients[i].connfd != 0 && strcmp(name, clients[i].username) == 0) {
            pthread_mutex_unlock(&clientsMutex);
            return -1;
        }
    }
    pthread_mutex_unlock(&clientsMutex);
    return 1;
}

int acceptClient(int *maxfd, fd_set *allReads) {
    // check if there's space to accept a new client
    pthread_mutex_lock(&clientsMutex);
    int i = 0;
    for (; i < numClients; i++) {
        if (clients[i].connfd == 0)
            break;
    }
    pthread_mutex_unlock(&clientsMutex);
    int connfd = accept(listenfd, NULL, NULL);
    if (connfd == -1) {
        if (errno != EINTR)
            perror("accept() failed");
        return 0;
    }
    if (i == numClients) {
        char *fullServer = "Server is full\n";
        tcpSend(fullServer, strlen(fullServer), connfd);
        close(connfd);
        return 1;
    }

    // prompt for username
    char *welcome = "Welcome to Chatroom! Please enter your username:\n";
    if (tcpSend(welcome, strlen(welcome), connfd) == -1) {
        close(connfd);
        return 1;
    }
    char username[MAX_NAME + 1];
    if (tcpRecv(username, MAX_NAME + 1, connfd) == -1) {
        close(connfd);
        return 1;
    }

    // repeat until unique username is 
    int n;
    while ((n = validName(i, username)) != 1) {
        char invalid[87 + strlen(username) + 1];
        if (n == -2) 
            sprintf(invalid, "Username \"%s\" is invalid (can only contain letters, digits, and underscores). Try another:\n", username);
        else 
            sprintf(invalid, "Username \"%s\" is already in use. Try another:\n", username);
        
        if (tcpSend(invalid, strlen(invalid), connfd) == -1) {
            close(connfd);
            return 1;
        }
        if (tcpRecv(username, MAX_NAME + 1, connfd) == -1) {
            close(connfd);
            return 1;
        }
    }

    // send confirmation to user
    char startChatting[24 + strlen(username) + 1];
    sprintf(startChatting, "Let's start chatting, %s!\n", username);
    if (tcpSend(startChatting, strlen(startChatting), connfd) == -1) {
        close(connfd);
        return 1;
    }

    // announce to everyone else user has joined
    char announceJoin[strlen(username) + 18 + 1];
    sprintf(announceJoin, "%s joined the chat.\n", username);

    pthread_mutex_lock(&clientsMutex);
    for (int j = 0; j < numClients; j++) {
        if (clients[j].connfd != 0 && tcpSend(announceJoin, strlen(announceJoin), clients[j].connfd) == -1) {
            close(clients[j].connfd);
            FD_CLR(clients[j].connfd, allReads);
            clients[j].connfd = 0;
        }
    }
    pthread_mutex_unlock(&clientsMutex);

    // keep track of connfd and mark client spot as taken
    if (connfd >= *maxfd)
        (*maxfd)++;
    FD_SET(connfd, allReads);
    pthread_mutex_lock(&clientsMutex);
    clients[i].connfd = connfd;
    strcpy(clients[i].username, username);
    pthread_mutex_unlock(&clientsMutex);
    return 1;
}


/* ---------------- Worker Thread Functions ---------------- */
// return 1 if everything succeeded, 0 if error
int createQuitMsg(Job *job, Job *bcastJob) {
    bcastJob->msg = malloc(strlen(job->username) + 20 + 1);
    if (bcastJob->msg == NULL) {
        free(job->msg);
        free(job);
        free(bcastJob);
        perror("malloc() failed");
        return 0;
    }
    bcastJob->quit = 1; //disconnect client after broadcasting message

    sprintf(bcastJob->msg, "%s has left the chat.\n", bcastJob->username);

    return 1;
}

int createWhoMsg(Job *job, Job *bcastJob) {
    int msgLen = 0;
    pthread_mutex_lock(&clientsMutex);
    for (int i = 0; i < numClients; i++) {
        if (clients[i].connfd != 0) {
            msgLen += strlen(clients[i].username) + 4; //" - username\n" for each connected client
        }
    }
    pthread_mutex_unlock(&clientsMutex);

    if (msgLen == 0) { //user who sent /who disconnected and no one else is connected -> treat as an error
        free(job->msg);
        free(job);
        return 0;
    }
    
    bcastJob->msg = malloc(14 + msgLen + 1);
    if (bcastJob->msg == NULL) {
        free(job->msg);
        free(job);
        free(bcastJob);
        perror("malloc() failed");
        return 0;
    }
    bcastJob->broadcast = 0; //only send to requester

    pthread_mutex_lock(&clientsMutex);
    sprintf(bcastJob->msg, "Active users:\n");
    int ind = 14;
    for (int i = 0; i < numClients; i++) {
        if (clients[i].connfd != 0) {
            sprintf(bcastJob->msg + ind, " - %s\n", clients[i].username);
            ind += strlen(clients[i].username) + 4;
        }
    }
    pthread_mutex_unlock(&clientsMutex);

    return 1;
}

int createMeMsg(Job *job, Job *bcastJob) {
    bcastJob->msg = malloc(strlen(job->username) + strlen(job->msg + 4) + 4 + 1);
    if (bcastJob->msg == NULL) {
        free(job->msg);
        free(job);
        free(bcastJob);
        perror("malloc() failed");
        return 0;
    }

    sprintf(bcastJob->msg, "*%s %s*\n", job->username, job->msg + 4);

    return 1;
}

int createErrMsg(Job *job, Job *bcastJob) {
    bcastJob->msg = malloc(43 + 1);
    if (bcastJob->msg == NULL) {
        free(job->msg);
        free(job);
        free(bcastJob);
        perror("malloc() failed");
        return 0;
    }
    bcastJob->broadcast = 0; //only send to requester

    sprintf(bcastJob->msg, "Invalid command. Type /who, /me, or /quit.\n");

    return 1;
}

int createMsg(Job *job, Job *bcastJob) {
    bcastJob->msg = malloc(strlen(job->username) + strlen(job->msg) + 3 + 1);
    if (bcastJob->msg == NULL) {
        free(job->msg);
        free(job);
        free(bcastJob);
        perror("malloc() failed");
        return 0;
    }

    sprintf(bcastJob->msg, "%s: %s\n", job->username, job->msg);

    return 1;
}

void *worker(__attribute__((unused)) void *arg) {
    pthread_mutex_lock(&jobQueue.mtx);
    while (!jobQueue.closed) {
        Job *job = q_pop(&jobQueue);
        pthread_mutex_unlock(&jobQueue.mtx); //unlock after popping since jobQueue isnt used in rest of loop so other threads can access it safely
        if (job == NULL) { //empty queue
            pthread_cond_wait(&jobQueue.cv, &jobQueue.mtx); //unlock, wait for signal, lock
            continue;
        }

        trimStr(job->msg);
        if (strlen(job->msg) == 0) { //ignore empty message
            free(job->msg);
            free(job);
            pthread_mutex_lock(&jobQueue.mtx);
            continue;
        }

        Job *bcastJob = malloc(sizeof(Job));
        if (bcastJob == NULL) {
            free(job->msg);
            free(job);
            perror("malloc() failed");
            pthread_mutex_lock(&jobQueue.mtx);
            continue;
        }
        bcastJob->username = job->username;
        bcastJob->sender_fd = job->sender_fd;
        bcastJob->broadcast = 1;
        bcastJob->quit = 0;

        // commands
        if (job->msg[0] == '/') {
            if (strcmp(job->msg, "/quit") == 0) {
                if (createQuitMsg(job, bcastJob) == 0) {
                    pthread_mutex_lock(&jobQueue.mtx);
                    continue;
                }
            } else if (strcmp(job->msg, "/who") == 0) {
                if (createWhoMsg(job, bcastJob) == 0) {
                    pthread_mutex_lock(&jobQueue.mtx);
                    continue;
                }
            } else if (strlen(job->msg) > 4 && strncmp(job->msg, "/me ", 4) == 0) {
                if (createMeMsg(job, bcastJob) == 0) {
                    pthread_mutex_lock(&jobQueue.mtx);
                    continue;
                }
            } else {
                if (createErrMsg(job, bcastJob) == 0) {
                    pthread_mutex_lock(&jobQueue.mtx);
                    continue;
                }
            }
        
        // regular messages
        } else {
            if (createMsg(job, bcastJob) == 0) {
                pthread_mutex_unlock(&jobQueue.mtx);
                continue;
            }
        }

        pthread_mutex_lock(&bcastQueue.mtx);
        q_push(&bcastQueue, bcastJob);
        pthread_mutex_unlock(&bcastQueue.mtx);
        
        free(job->msg);
        free(job);
        pthread_mutex_lock(&jobQueue.mtx);
    }

    pthread_mutex_unlock(&jobQueue.mtx);
    return NULL;
}


/* ---------------- Main Thread ---------------- */
int main(int argc, char **argv) {
    // validate args
    if (argc != 4) {
        fprintf(stderr, "Invalid number of arguments\n");
        printUsage();
        return EXIT_FAILURE;
    }
    int port = validPort(argv[1]);
    if (port == -1) {
        fprintf(stderr, "Invalid port\n");
        printUsage();
        return EXIT_FAILURE;
    }
    numWorkers = validNumWorkers(argv[2]);
    if (numWorkers == -1) {
        fprintf(stderr, "Invalid num_workers\n");
        printUsage();
        return EXIT_FAILURE;
    }
    numClients = validNumClients(argv[3]);
    if (numClients == -1) {
        fprintf(stderr, "Invalid max_clients\n");
        printUsage();
        return EXIT_FAILURE;
    }

    // create socket and setup select()
    listenfd = listenSocket(port);
    if (listenfd == -1) {
        return EXIT_FAILURE;
    }

    // keep track of client fds + names
    clients = calloc(numClients, sizeof(Client)); //initialize all fds to 0
    if (clients == NULL) {
        close(listenfd);
        perror("calloc() failed");
        return EXIT_FAILURE;
    }
    pthread_mutex_init(&clientsMutex, NULL);

    // initialize queues
    q_init(&jobQueue);
    q_init(&bcastQueue);

    // create worker threads
    workers = malloc(sizeof(pthread_t) * numWorkers);
    if (workers == NULL) {
        close(listenfd);
        free(clients);
        perror("malloc() failed");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < numWorkers; i++) {
        pthread_t tid;
        int n;
        if ((n = pthread_create(&tid, NULL, worker, NULL)) != 0) {
            close(listenfd);
            free(clients);
            free(workers);
            q_close(&jobQueue);
            pthread_cond_broadcast(&jobQueue.cv);
            for (int j = 0; j < i; j++)
                pthread_join(workers[i], NULL);
            fprintf(stderr, "pthread_create() failed: %d\n", n);
            return EXIT_FAILURE;
        }
        workers[i] = tid;
    }

    // setup graceful termination
    struct sigaction act;
    act.sa_handler = shutdownServer;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    if (sigaction(SIGINT, &act, NULL) == -1) {
        close(listenfd);
        free(clients);
        free(workers);
        q_close(&jobQueue);
        pthread_cond_broadcast(&jobQueue.cv);
        for (int i = 0; i < numWorkers; i++) 
            pthread_join(workers[i], NULL);
        perror("sigaction() failed");
        return EXIT_FAILURE;
    }

    fd_set readfds, allReads;
    FD_ZERO(&allReads);
    FD_SET(listenfd, &allReads);
    int maxfd = listenfd+1;

    // server loop
    while (!shutdownFlag) {
        readfds = allReads;
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        if (select(maxfd, &readfds, NULL, NULL, &timeout) == -1) { //set up a timeout
            if (errno == EINTR)
                break;
            cleanup();
            perror("select() failed");
            return EXIT_FAILURE;
        }

        if (FD_ISSET(listenfd, &readfds) && !acceptClient(&maxfd, &allReads)) {
            if (errno == EINTR) 
                break;
            cleanup();
            return EXIT_FAILURE;
        }

        pthread_mutex_lock(&clientsMutex);
        for (int i = 0; i < numClients; i++) {
            if (clients[i].connfd != 0 && FD_ISSET(clients[i].connfd, &readfds)) {
                Job *job = malloc(sizeof(Job));
                if (job == NULL) {
                    if (errno == EINTR)
                        break;
                    pthread_mutex_unlock(&clientsMutex);
                    perror("malloc() failed");
                    cleanup();
                    return EXIT_FAILURE;
                }
                job->msg = malloc(MAX_MSG);
                if (job->msg == NULL) {
                    free(job);
                    if (errno == EINTR)
                        break;
                    pthread_mutex_unlock(&clientsMutex);
                    perror("malloc() failed");
                    cleanup();
                    return EXIT_FAILURE;
                }
                job->sender_fd = clients[i].connfd;
                job->username = clients[i].username;

                if (tcpRecv(job->msg, MAX_MSG, job->sender_fd) == -1) {
                    close(clients[i].connfd);
                    FD_CLR(clients[i].connfd, &allReads);
                    clients[i].connfd = 0;
                    free(job->msg);
                    free(job);
                    continue;
                }
                pthread_mutex_lock(&jobQueue.mtx);
                q_push(&jobQueue, job);
                pthread_mutex_unlock(&jobQueue.mtx);
            }
        }
        pthread_mutex_unlock(&clientsMutex);

        if (shutdownFlag)
            break;

        pthread_mutex_lock(&bcastQueue.mtx);
        pthread_mutex_lock(&clientsMutex);
        Job *bcastJob = q_pop(&bcastQueue);
        while (bcastJob != NULL) {
            int i = 0; //if i == numClients, requester disconnected and message should be ignored
            for (; i < numClients; i++) {
                if (clients[i].connfd != 0 && clients[i].connfd == bcastJob->sender_fd)
                    break;
            }

            if (i != numClients && bcastJob->broadcast == 0) { //only send to requester
                if (tcpSend(bcastJob->msg, strlen(bcastJob->msg), clients[i].connfd) == -1) {
                    close(clients[i].connfd);
                    clients[i].connfd = 0;
                }
            } else if (i != numClients) { //send to all but requester
                for (int j = 0; j < numClients; j++) {
                    if (i != j && clients[j].connfd != 0 && 
                            tcpSend(bcastJob->msg, strlen(bcastJob->msg), clients[j].connfd) == -1) {
                        
                        close(clients[j].connfd);
                        FD_CLR(clients[i].connfd, &allReads);
                        clients[j].connfd = 0;
                    }
                }
                if (bcastJob->quit == 1) {
                    close(clients[i].connfd);
                    FD_CLR(clients[i].connfd, &allReads);
                    clients[i].connfd = 0;
                }
            }

            free(bcastJob->msg);
            free(bcastJob);
            bcastJob = q_pop(&bcastQueue);
        }
        pthread_mutex_unlock(&bcastQueue.mtx);
        pthread_mutex_unlock(&clientsMutex);
    }
    
    cleanup();
    return EXIT_SUCCESS;
}