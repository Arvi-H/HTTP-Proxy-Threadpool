#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h> 
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <pthread.h> 
#include <semaphore.h>

/* Recommended max object size */
#define MAX_OBJECT_SIZE 102400
#define BUFFER_SIZE 5
#define THREAD_POOL_SIZE 8

static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:97.0) Gecko/20100101 Firefox/97.0";
sem_t items, spaces;
pthread_mutex_t mutex;
char buffer[MAX_OBJECT_SIZE];
int in = 0, out = 0; 

int complete_request_received(char *);
int parse_request(char *, char *, char *, char *, char *);
void test_parser();
void print_bytes(unsigned char *, int);
void handle_client(int);
void construct_proxied_request(char *new_request, char *method, char *hostname, char *port, char *path);
int open_sfd(int);
void *client_handling_thread(void *thread_arg);

int main(int argc, char *argv[]) {
    // Server socket file descriptor
    int server_fd; 
    
    // Client socket file descriptor
    int client_fd;

    // Client address
    struct sockaddr_in client_addr;

    // Size of client address
    socklen_t client_addr_len = sizeof(client_addr);
    
    // Validate command line arguments
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Open server socket
    server_fd = open_sfd(atoi(argv[1]));

    // Initialize semaphores and mutex
    sem_init(&items, 0, 0);
    sem_init(&spaces, 0, BUFFER_SIZE);
    pthread_mutex_init(&mutex, NULL);

    // Start thread pool
    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&threads[i], NULL, client_handling_thread, NULL);
    }

    // Accept client connections
    while (1) {
        // Accept new client
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd == -1) {
            perror("Accept");
            continue;
        }

        // Add client to queue
        sem_wait(&spaces);  
        pthread_mutex_lock(&mutex); 

        buffer[in] = client_fd; 
        in = (in + 1) % BUFFER_SIZE;  

        pthread_mutex_unlock(&mutex);   
        sem_post(&items); 
    }

    // Close server socket
    close(server_fd);
    
    return 0;
}


void handle_client(int client_socket) {
    char client_request_buffer[MAX_OBJECT_SIZE], proxied_request[MAX_OBJECT_SIZE], server_response[MAX_OBJECT_SIZE];
    int bytes_read, server_bytes_received, response_bytes_sent;
    char request_method[16], server_host[64], server_port[8], request_path[64];

    // Loop to receive the full HTTP request from client
    memset(client_request_buffer, 0, MAX_OBJECT_SIZE);
    while ((bytes_read = recv(client_socket, client_request_buffer + strlen(client_request_buffer), MAX_OBJECT_SIZE - strlen(client_request_buffer) - 1, 0)) > 0) {
        client_request_buffer[bytes_read + strlen(client_request_buffer)] = '\0';
        if (complete_request_received(client_request_buffer)) { break; }
    }

    // Parse client HTTP request into components
    if (!parse_request(client_request_buffer, request_method, server_host, server_port, request_path)) {
        close(client_socket);
        return;
    }

    // Construct proxied HTTP request to send to server
    construct_proxied_request(proxied_request, request_method, server_host, server_port, request_path);

    // Print proxied request for debugging 
    print_bytes((unsigned char *)proxied_request, strlen(proxied_request));

    struct addrinfo hints, *res;
    int server_socket;

    // Set up address info hints
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; 
    hints.ai_socktype = SOCK_STREAM;  

    char server_port_string[8];
    
    // Convert port to string for lookup
    snprintf(server_port_string, sizeof(server_port_string), "%s", strcmp(server_port, "80") == 0 ? "http" : server_port);

    // Lookup address info   
    if (getaddrinfo(server_host, server_port_string, &hints, &res) != 0) {
        perror("getaddrinfo");
        close(client_socket);
        return;
    }

    // Create server socket
    server_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_socket == -1) {
        perror("socket");
        freeaddrinfo(res);
        close(client_socket);
        return;
    }

    // Connect to server
    if (connect(server_socket, res->ai_addr, res->ai_addrlen) == -1) {
        perror("connect");
        close(server_socket);
        freeaddrinfo(res);
        close(client_socket);
        return;
    }
 
    // Free address info  
    freeaddrinfo(res);
 
    // Send request to server 
    if (send(server_socket, proxied_request, strlen(proxied_request), 0) == -1) {
        perror("send");
        close(server_socket);
        close(client_socket);
        return;
    }
 
    // Receive response in loop
    while ((server_bytes_received = recv(server_socket, server_response, MAX_OBJECT_SIZE - 1, 0)) > 0) {
        // Send response to client
        response_bytes_sent = send(client_socket, server_response, server_bytes_received, 0);
        if (response_bytes_sent == -1) {
            perror("send to client");
            break;  
        }
    }
    
    // Check for recv error
    if (server_bytes_received == -1) {
        perror("Error");
    }

    // Close the client and server sockets
    close(server_socket);
    close(client_socket);
} 

void construct_proxied_request(char *new_request, char *method, char *hostname, char *port, char *path) {
    char host_header[100];
    if (strcmp(port, "80") != 0) {
        snprintf(host_header, sizeof(host_header), "%s:%s", hostname, port); 
    } else {
        snprintf(host_header, sizeof(host_header), "%s", hostname);
    }

    snprintf(new_request, MAX_OBJECT_SIZE,  
             "%s %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "%s\r\n"
             "Connection: close\r\n"
             "Proxy-Connection: close\r\n\r\n",
             method, path, host_header, user_agent_hdr);
}

// Function to create and configure a TCP socket for listening
int open_sfd(int port) {
    // Create a TCP socket
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == -1) {
        perror("Error creating socket");
        exit(EXIT_FAILURE);
    }

    // Allow the socket to bind to an address and port already in use
    int optval = 1;
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) == -1) {
        perror("Error setting socket options");
        close(sfd);
        exit(EXIT_FAILURE);
    }

    // Configure the server address structure
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr)); 
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // Bind the socket to the specified port
    if (bind(sfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Error binding socket");
        close(sfd);
        exit(EXIT_FAILURE);
    }

    // Configure the socket for accepting new clients
    if (listen(sfd, SOMAXCONN) == -1) {
        perror("Error listening on socket");
        close(sfd);
        exit(EXIT_FAILURE);
    }

    return sfd;
}

void *client_handling_thread(void *thread_arg) { 
  while (1) {
    int client_fd;

    // Wait for item
    sem_wait(&items);
    pthread_mutex_lock(&mutex);

    // Get client file descriptor from queue
    client_fd = buffer[out];
    out = (out + 1) % BUFFER_SIZE;

    pthread_mutex_unlock(&mutex);
    sem_post(&spaces);
    
    // Handle client request
    handle_client(client_fd);    
  }
}

int complete_request_received(char *request) {
	// Use strstr to find the end-of-headers sequence "\r\n\r\n"
    // If the sequence is found, the request is complete  
    return (strstr(request, "\r\n\r\n") != NULL) ? 1 : 0;
}

int parse_request(char *request, char *method, char *hostname, char *port, char *path) {
    // Extract method
    char *beginning = request;
    // Space indicates end of method
    char *end = strstr(beginning, " ");
    // Copy method
    strncpy(method, beginning, end - beginning);
    method[end - beginning] = '\0';

    // Move beyond the first space to start of the URL
    beginning = end + 1;

    // Extract URL
    // Find next space
    end = strstr(beginning, " ");
    char url[1024];                      
    strncpy(url, beginning, end - beginning);
    url[end - beginning] = '\0';

    char *url_beginning = strstr(url, "://");
    if (!url_beginning) return 0;  
    url_beginning += 3;  

    // Extract hostname, port, and path from the URL
    char *colon_position = strstr(url_beginning, ":");
    char *slash_position = strstr(url_beginning, "/");
    
    // Check if a colon is present in the URL after "://" and if there is a slash after the colon
    if (colon_position != NULL && slash_position && colon_position < slash_position) {
        // Extract and copy the hostname from the URL
        strncpy(hostname, url_beginning, colon_position - url_beginning);
        hostname[colon_position - url_beginning] = '\0'; 
        
        // Extract and copy the port from the URL
        strncpy(port, colon_position + 1, slash_position - (colon_position + 1));
        port[slash_position - (colon_position + 1)] = '\0'; 
    } else {
        // If no colon or the colon is after the slash, use default port 80
        // Extract and copy the hostname from the URL
        strncpy(hostname, url_beginning, slash_position - url_beginning);
        hostname[slash_position - url_beginning] = '\0'; 

        // Set the port to the default value "80"
        strcpy(port, "80");
    }

    // Copy the path from the URL (including the leading "/")
    strcpy(path, slash_position);

    // Check if the entire request has been received
    return complete_request_received(request);
}

void test_parser() {
	int i;
	char method[16], hostname[64], port[8], path[64];

       	char *reqs[] = {
		"GET http://www.example.com/index.html HTTP/1.0\r\n"
		"Host: www.example.com\r\n"
		"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:68.0) Gecko/20100101 Firefox/68.0\r\n"
		"Accept-Language: en-US,en;q=0.5\r\n\r\n",

		"GET http://www.example.com:8080/index.html?foo=1&bar=2 HTTP/1.0\r\n"
		"Host: www.example.com:8080\r\n"
		"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:68.0) Gecko/20100101 Firefox/68.0\r\n"
		"Accept-Language: en-US,en;q=0.5\r\n\r\n",

		"GET http://localhost:1234/home.html HTTP/1.0\r\n"
		"Host: localhost:1234\r\n"
		"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:68.0) Gecko/20100101 Firefox/68.0\r\n"
		"Accept-Language: en-US,en;q=0.5\r\n\r\n",

		"GET http://www.example.com:8080/index.html HTTP/1.0\r\n",

		NULL
	};
	
	for (i = 0; reqs[i] != NULL; i++) {
		printf("Testing %s\n", reqs[i]);
		if (parse_request(reqs[i], method, hostname, port, path)) {
			printf("METHOD: %s\n", method);
			printf("HOSTNAME: %s\n", hostname);
			printf("PORT: %s\n", port);
			printf("PATH: %s\n", path);
		} else {
			printf("REQUEST INCOMPLETE\n");
		}
	}
}

void print_bytes(unsigned char *bytes, int byteslen) {
	int i, j, byteslen_adjusted;

	if (byteslen % 8) {
		byteslen_adjusted = ((byteslen / 8) + 1) * 8;
	} else {
		byteslen_adjusted = byteslen;
	}
	for (i = 0; i < byteslen_adjusted + 1; i++) {
		if (!(i % 8)) {
			if (i > 0) {
				for (j = i - 8; j < i; j++) {
					if (j >= byteslen_adjusted) {
						printf("  ");
					} else if (j >= byteslen) {
						printf("  ");
					} else if (bytes[j] >= '!' && bytes[j] <= '~') {
						printf(" %c", bytes[j]);
					} else {
						printf(" .");
					}
				}
			}
			if (i < byteslen_adjusted) {
				printf("\n%02X: ", i);
			}
		} else if (!(i % 4)) { 
			printf(" ");
		}
		if (i >= byteslen_adjusted) {
			continue;
		} else if (i >= byteslen) {
			printf("   ");
		} else {
			printf("%02X ", bytes[i]);
		}
	}
	printf("\n");
}
