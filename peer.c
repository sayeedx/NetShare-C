#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h> // For stat() to check file existence
#include <fcntl.h>
#include <errno.h>
#include <dirent.h> // For directory scanning (for de-register on quit)

#define PDU_DATA_SIZE 1500      // Increased to match server
#define TCP_CHUNK_SIZE 1399     // Byte-limited to respect network limits for TCP data transfer
#define NAME_LEN 10             // Max length for peer and content names
#define RESERVED_SERVER_NAME "IS" // Reserved name for the Index Server
#define MAX_MY_CONTENT 10       // Maximum number of content files a single peer can register/serve

// PDU structure: Used for communication between peer and index server, and for TCP data transfer.
struct pdu {
    char type; // Type of PDU
    char data[PDU_DATA_SIZE]; // Payload data
};

// Structure to hold information about content files that this peer owns and serves.
struct my_content_info {
    char content_name[NAME_LEN + 1]; // Name of the content file (e.g., "file1.txt")
    char file_path[256];             // Full path to the local content file
    int tcp_listen_socket;           // TCP socket file descriptor for listening for download requests for this content
    uint16_t tcp_port_net;           // TCP port in network byte order on which this content is served
    int is_active;                   // Flag: 1 if this content is currently active and being served, 0 otherwise
};

// Global variables for the Peer application.
struct my_content_info my_contents[MAX_MY_CONTENT]; // Array to store info about owned content
int my_content_count = 0;                          // Current number of owned content files
char peer_global_name[NAME_LEN + 1] = "DefaultPeer"; // This peer's unique name
int udp_sock;                                      // UDP socket for communication with the Index Server
struct sockaddr_in server_addr_udp;                // Address of the Index Server for UDP communication

// Global variables for download connection reuse
int last_download_socket = -1;                     // Stores the FD of the most recently used TCP download socket
struct sockaddr_in last_download_target_addr;      // Stores the address of the target of the last download
int is_download_socket_active = 0;                 // Flag to indicate if the last download socket is still open

// Peer state management
typedef enum {
    NORMAL_COMMAND,             // Peer is waiting for a command from stdin
    WAITING_FOR_DOWNLOAD_CHOICE,// Peer is waiting for "yes/no" to download
    WAITING_FOR_REUSE_CHOICE    // Peer is waiting for "yes/no" to reuse connection
} PeerState;

PeerState current_peer_state = NORMAL_COMMAND; // Initial state

// Buffers to store information for pending prompts (so state machine can access them)
char pending_download_content_name[NAME_LEN + 1];
struct sockaddr_in pending_download_target_addr;
char pending_download_ip_str[INET_ADDRSTRLEN];
int pending_download_port;


// Function to clear any remaining characters in stdin buffer, including the newline.
// This is crucial to prevent fgets from consuming unintended leftover input.
void clear_stdin_buffer() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}


// Helper function to send a PDU to the configured Index Server via UDP.
// type: The type character of the PDU.
// data_payload: Pointer to the data payload.
// data_len: Length of the data payload.
void send_pdu_to_idx_server(char type, const char *data_payload, int data_len) {
    struct pdu spdu; // PDU to send
    spdu.type = type;
    if (data_payload) {
        // Copy data payload if provided, respecting max size.
        memcpy(spdu.data, data_payload, data_len < PDU_DATA_SIZE ? data_len : PDU_DATA_SIZE);
        // Send the PDU. The UDP socket is already connected to the server.
        send(udp_sock, &spdu, sizeof(char) + data_len, 0);
    } else {
        // If no data payload, send only the type.
        send(udp_sock, &spdu, sizeof(char), 0);
    }
    printf("Sent PDU type '%c' to Index Server.\n", type);
}

// Sets up a TCP listening socket for a specific content file.
// content: Pointer to the my_content_info structure for the content.
// Returns the listening socket file descriptor on success, -1 on failure.
int setup_tcp_listen_socket(struct my_content_info *content) {
    int listen_sock_fd;
    struct sockaddr_in tcp_server_addr; // Address structure for the TCP listening socket

    // Create a TCP socket.
    if ((listen_sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Error creating TCP socket");
        return -1;
    }
    
    // Initialize TCP server address structure.
    memset(&tcp_server_addr, 0, sizeof(tcp_server_addr));
    tcp_server_addr.sin_family = AF_INET;
    tcp_server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all available interfaces
    tcp_server_addr.sin_port = htons(0); // Let the OS assign a random available port (ephemeral port)

    // Bind the TCP socket to the address.
    if (bind(listen_sock_fd, (struct sockaddr *)&tcp_server_addr, sizeof(tcp_server_addr)) < 0) {
        perror("Error binding TCP socket");
        close(listen_sock_fd);
        return -1;
    }
    // Start listening for incoming TCP connections.
    if (listen(listen_sock_fd, 5) < 0) { // 5 is the backlog queue size
        perror("Error listening on TCP socket");
        close(listen_sock_fd);
        return -1;
    }

    // Get the dynamically assigned port number from the OS.
    socklen_t len = sizeof(tcp_server_addr);
    if (getsockname(listen_sock_fd, (struct sockaddr *)&tcp_server_addr, &len) == -1) {
        perror("Error getting socket name (TCP port)");
        close(listen_sock_fd);
        return -1;
    }

    // Store the obtained port and socket in the content info structure.
    content->tcp_port_net = tcp_server_addr.sin_port; // Port in network byte order
    content->tcp_listen_socket = listen_sock_fd;
    content->is_active = 1; // Mark this content as active and being served

    printf("Content '%.10s' listening on TCP port %d\n", content->content_name, ntohs(content->tcp_port_net));
    return listen_sock_fd;
}

// Handles an incoming TCP download request from another peer.
// client_sock: The connected TCP socket with the requesting peer.
// serving_content: Pointer to the my_content_info of the file being requested.
void handle_tcp_download_request(int client_sock, const struct my_content_info* serving_content) {
    struct pdu dpdu; // PDU received from the client (should be a 'D' PDU)
    ssize_t n_recv;
    FILE *fp = NULL; // File pointer for the content file

    // Receive the 'D' PDU (Download Request) from the client.
    n_recv = recv(client_sock, &dpdu, sizeof(dpdu), 0);
    if (n_recv <= 0) {
        fprintf(stderr, "Error or no data received for D-PDU. Closing connection.\n");
        close(client_sock);
        return;
    }

    // Check if it's a valid 'D' PDU for the expected content.
    if (dpdu.type == 'D' && strncmp(dpdu.data, serving_content->content_name, NAME_LEN) == 0) {
        printf("Valid download request for '%.10s'. Starting transfer.\n", serving_content->content_name);
        
        // Open the requested file in binary read mode.
        fp = fopen(serving_content->file_path, "rb");
        if (!fp) {
            perror("Error opening file for download");
            // Optionally send an error PDU back to the client if the file cannot be opened.
            close(client_sock);
            return;
        }

        char c_pdu_type = 'C'; // 'C' PDU type to indicate content transfer is starting.
        // Send 'C' PDU type to the client to confirm readiness to send data.
        send(client_sock, &c_pdu_type, 1, 0);

        char file_buffer[TCP_CHUNK_SIZE]; // Buffer for reading file chunks
        size_t bytes_read;
        printf("Receiving content...\n");
        // Read file in chunks and send them over the TCP socket.
        while ((bytes_read = fread(file_buffer, 1, TCP_CHUNK_SIZE, fp)) > 0) {
            if (send(client_sock, file_buffer, bytes_read, 0) < bytes_read) {
                perror("Error sending file data");
                break; // Exit loop on send error
            }
        }
        fclose(fp); // Close the file after transfer.
        printf("Transfer of '%.10s' complete.\n", serving_content->content_name);
    } else {
        fprintf(stderr, "Invalid D-PDU received (type '%c' or wrong content name). Closing connection.\n", dpdu.type);
    }
    close(client_sock); // Close the client socket after handling the request.
}


int main(int argc, char *argv[]) {
    char *server_ip = "127.0.0.1"; // Default Index Server IP
    int server_port_idx = 7000;    // Default Index Server UDP port
    fd_set read_fds, active_fds;   // File descriptor sets for select()
    int max_fd;                    // Maximum file descriptor value for select()

    // Command line argument parsing for peer name, server IP, and server port.
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <YourPeerName> [ServerIP] [ServerPort]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    strncpy(peer_global_name, argv[1], NAME_LEN);
    peer_global_name[NAME_LEN] = '\0'; // Ensure null termination
    
    // Check if the chosen peer name is reserved.
    if (strcmp(peer_global_name, RESERVED_SERVER_NAME) == 0) {
        fprintf(stderr, "Error: '%s' is a reserved name. Please choose another name.\n", RESERVED_SERVER_NAME);
        exit(EXIT_FAILURE);
    }

    // Parse optional server IP and port.
    if (argc > 2) server_ip = argv[2];
    if (argc > 3) server_port_idx = atoi(argv[3]);
    
    // UDP socket creation and connection to Index Server.
    if ((udp_sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Error creating UDP socket");
        exit(EXIT_FAILURE);
    }
    memset(&server_addr_udp, 0, sizeof(server_addr_udp));
    server_addr_udp.sin_family = AF_INET;
    server_addr_udp.sin_port = htons(server_port_idx);
    // Convert server IP string to network address.
    if (inet_pton(AF_INET, server_ip, &server_addr_udp.sin_addr) <= 0) {
        perror("Invalid server IP address");
        close(udp_sock);
        exit(EXIT_FAILURE);
    }
    // Connect the UDP socket to the server's address. This allows using send() instead of sendto().
    if (connect(udp_sock, (struct sockaddr *)&server_addr_udp, sizeof(server_addr_udp)) < 0) {
        perror("Error connecting UDP socket to server");
        close(udp_sock);
        exit(EXIT_FAILURE);
    }

    // Initialize file descriptor sets for select().
    FD_ZERO(&active_fds); // Clear all FDs in the set
    FD_SET(STDIN_FILENO, &active_fds); // Add standard input (keyboard) to the set
    FD_SET(udp_sock, &active_fds);     // Add UDP socket to the set
    max_fd = udp_sock;                 // Initialize max_fd with the UDP socket FD

    printf("Peer '%.10s' started.\n", peer_global_name);
    printf("Commands: register <content_name> <file_path>, search <content_name>, list, deregister <content_name>, quit\n");

    // Main loop for handling user commands and network events.
    // This loop demonstrates the peer acting as both a client (for commands like search/list)
    // and a server (for serving files via TCP) simultaneously using select().
    while (1) {
        read_fds = active_fds; // Copy active_fds to read_fds for each select() call
        
        // Only print prompt if we are not currently waiting for a prompt response
        if (current_peer_state == NORMAL_COMMAND) {
            printf("\n%s> ", peer_global_name); // Prompt for user input
        } else {
            // Re-print prompt if it was lost due to asynchronous network output
            if (current_peer_state == WAITING_FOR_DOWNLOAD_CHOICE) {
                printf("Do you want to download '%.10s'? (yes/no): ", pending_download_content_name);
            } else if (current_peer_state == WAITING_FOR_REUSE_CHOICE) {
                 printf("Reuse existing connection to %s:%d? (yes/no): ", pending_download_ip_str, pending_download_port);
            }
        }
        fflush(stdout); // Ensure the prompt is displayed immediately

        // Use select() to monitor multiple file descriptors for readability.
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("select failed");
            exit(EXIT_FAILURE);
        }

        // --- Handle user input from STDIN (peer as client) ---
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char input_buffer[512]; // Buffer for any stdin input (command or choice)
            if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) {
                fprintf(stderr, "Error reading from stdin or EOF. Quitting.\n");
                break;
            }
            input_buffer[strcspn(input_buffer, "\n")] = 0; // Remove trailing newline
            
            // Handle input based on current state
            if (current_peer_state == NORMAL_COMMAND) {
                char command[32];       // Buffer for the command token
                char arg1[NAME_LEN + 1];  // Buffer for first argument (content name)
                char arg2[256];         // Buffer for second argument (file path)
                
                int items = sscanf(input_buffer, "%s %s %s", command, arg1, arg2);

                if (strcmp(command, "register") == 0) {
                    if (items < 3) {
                        printf("Usage: register <content_name> <file_path>\n");
                        continue;
                    }
                    if (strlen(arg1) > NAME_LEN) {
                        printf("Error: Content name '%s' exceeds max length of %d characters.\n", arg1, NAME_LEN);
                        continue;
                    }
                    if (strlen(arg2) >= 256) {
                        printf("Error: File path '%s' exceeds max length of 255 characters.\n", arg2);
                        continue;
                    }
                    
                    struct stat file_stat;
                    if (stat(arg2, &file_stat) != 0) {
                        perror("Error: Local file does not exist or cannot be accessed");
                        continue;
                    }

                    if (my_content_count >= MAX_MY_CONTENT) {
                        printf("Error: Cannot register more than %d content files.\n", MAX_MY_CONTENT);
                        continue;
                    }

                    int content_slot_idx = -1;
                    for (int i = 0; i < MAX_MY_CONTENT; i++) {
                        if (!my_contents[i].is_active) {
                            content_slot_idx = i;
                            break;
                        }
                    }
                    if (content_slot_idx == -1) {
                        printf("Error: No available slot for new content.\n");
                        continue;
                    }

                    strncpy(my_contents[content_slot_idx].content_name, arg1, NAME_LEN);
                    my_contents[content_slot_idx].content_name[NAME_LEN] = '\0';
                    strncpy(my_contents[content_slot_idx].file_path, arg2, sizeof(my_contents[content_slot_idx].file_path) - 1);
                    my_contents[content_slot_idx].file_path[sizeof(my_contents[content_slot_idx].file_path) - 1] = '\0';
                    
                    int new_tcp_sock = setup_tcp_listen_socket(&my_contents[content_slot_idx]);
                    if (new_tcp_sock < 0) {
                        printf("Error: Failed to set up TCP listen socket for '%s'.\n", arg1);
                        my_contents[content_slot_idx].is_active = 0;
                        continue;
                    }
                    
                    FD_SET(new_tcp_sock, &active_fds);
                    if (new_tcp_sock > max_fd) {
                        max_fd = new_tcp_sock;
                    }

                    char r_pdu_data[PDU_DATA_SIZE];
                    memcpy(r_pdu_data, peer_global_name, NAME_LEN);
                    memcpy(r_pdu_data + NAME_LEN, arg1, NAME_LEN);
                    struct sockaddr_in local_tcp_addr;
                    socklen_t addr_len = sizeof(local_tcp_addr);
                    if (getsockname(new_tcp_sock, (struct sockaddr *)&local_tcp_addr, &addr_len) == -1) {
                        perror("getsockname for local TCP address failed");
                    }
                    memcpy(r_pdu_data + (2 * NAME_LEN), &local_tcp_addr, sizeof(struct sockaddr_in));
                    
                    send_pdu_to_idx_server('R', r_pdu_data, (2 * NAME_LEN) + sizeof(struct sockaddr_in));
                    
                    my_content_count++;
                    printf("Attempting to register content '%.10s' with Index Server.\n", arg1);

                } else if (strcmp(command, "deregister") == 0) {
                    if (items < 2) {
                        printf("Usage: deregister <content_name>\n");
                        continue;
                    }
                    if (strlen(arg1) > NAME_LEN) {
                        printf("Error: Content name '%s' exceeds max length of %d characters.\n", arg1, NAME_LEN);
                        continue;
                    }

                    int found_content_idx = -1;
                    for (int i = 0; i < MAX_MY_CONTENT; i++) {
                        if (my_contents[i].is_active && strncmp(my_contents[i].content_name, arg1, NAME_LEN) == 0) {
                            found_content_idx = i;
                            break;
                        }
                    }

                    if (found_content_idx != -1) {
                        char t_pdu_data[PDU_DATA_SIZE];
                        memcpy(t_pdu_data, peer_global_name, NAME_LEN);
                        memcpy(t_pdu_data + NAME_LEN, arg1, NAME_LEN);
                        send_pdu_to_idx_server('T', t_pdu_data, 2 * NAME_LEN);

                        my_contents[found_content_idx].is_active = 0;
                        close(my_contents[found_content_idx].tcp_listen_socket);
                        FD_CLR(my_contents[found_content_idx].tcp_listen_socket, &active_fds);
                        printf("De-registering '%.10s' from Index Server and local service.\n", arg1);
                        my_content_count--;
                    } else {
                        printf("Error: Content '%.10s' is not currently registered by this peer.\n", arg1);
                    }
                } else if (strcmp(command, "search") == 0) {
                    if (items < 2) {
                        printf("Usage: search <content_name>\n");
                        continue;
                    }
                    if (strlen(arg1) > NAME_LEN) {
                        printf("Error: Content name '%s' exceeds max length of %d characters.\n", arg1, NAME_LEN);
                        continue;
                    }
                    send_pdu_to_idx_server('S', arg1, NAME_LEN);
                } else if (strcmp(command, "list") == 0) {
                    send_pdu_to_idx_server('O', NULL, 0);
                } else if (strcmp(command, "quit") == 0) {
                    printf("Quitting. De-registering all active content from Index Server...\n");
                    if (is_download_socket_active) {
                        close(last_download_socket);
                        is_download_socket_active = 0;
                        last_download_socket = -1;
                    }

                    for (int i = 0; i < MAX_MY_CONTENT; ++i) {
                        if (my_contents[i].is_active) {
                            printf("  De-registering '%.10s'\n", my_contents[i].content_name);
                            char t_pdu_data[PDU_DATA_SIZE];
                            memcpy(t_pdu_data, peer_global_name, NAME_LEN);
                            memcpy(t_pdu_data + NAME_LEN, my_contents[i].content_name, NAME_LEN);
                            send_pdu_to_idx_server('T', t_pdu_data, 2 * NAME_LEN);
                            close(my_contents[i].tcp_listen_socket);
                            FD_CLR(my_contents[i].tcp_listen_socket, &active_fds);
                            my_contents[i].is_active = 0;
                            my_content_count--;
                            usleep(50000);
                        }
                    }
                    sleep(1);
                    printf("All content de-registered. Shutting down.\n");
                    break;
                } else {
                    printf("Unknown command or incorrect usage.\n");
                }
            } else if (current_peer_state == WAITING_FOR_DOWNLOAD_CHOICE) {
                // User responded to "Do you want to download?"
                int download_decision = 0; // 0: no download, 1: new connection, 2: reuse connection

                if (strcmp(input_buffer, "yes") == 0) {
                    // Check for connection reuse option
                    if (is_download_socket_active && 
                        last_download_target_addr.sin_addr.s_addr == pending_download_target_addr.sin_addr.s_addr &&
                        last_download_target_addr.sin_port == pending_download_target_addr.sin_port) {
                        
                        printf("Reuse existing connection to %s:%d? (yes/no): ", pending_download_ip_str, pending_download_port);
                        current_peer_state = WAITING_FOR_REUSE_CHOICE; // Change state for next input
                        continue; // Go back to select to wait for reuse choice
                    } else {
                        download_decision = 1; // Create new connection (no existing or target mismatch)
                    }
                } else {
                    printf("Download cancelled.\n");
                    if (is_download_socket_active) {
                        close(last_download_socket);
                        is_download_socket_active = 0;
                        last_download_socket = -1;
                    }
                    current_peer_state = NORMAL_COMMAND; // Reset state
                    continue; // Skip download logic
                }
                
                // If we reach here, either it's a new connection or we already decided to reuse
                // (if reuse prompt was skipped or user chose 'no' to reuse)
                if (download_decision == 1) { // Process new connection
                    // Close previous connection if it was active
                    if (is_download_socket_active) {
                        close(last_download_socket);
                        is_download_socket_active = 0;
                        last_download_socket = -1;
                    }

                    int dl_sock = socket(AF_INET, SOCK_STREAM, 0);
                    if (dl_sock < 0) {
                        perror("Error creating download TCP socket");
                        current_peer_state = NORMAL_COMMAND;
                        continue;
                    }
                    
                    if (connect(dl_sock, (struct sockaddr *)&pending_download_target_addr, sizeof(pending_download_target_addr)) < 0) {
                        perror("Error connecting to content server for download");
                        close(dl_sock);
                        current_peer_state = NORMAL_COMMAND;
                        continue;
                    }
                    printf("Created new connection to %s:%d.\n", pending_download_ip_str, pending_download_port);
                    last_download_socket = dl_sock;
                    last_download_target_addr = pending_download_target_addr;
                    is_download_socket_active = 1;
                    current_peer_state = NORMAL_COMMAND; // Reset state for next command
                    
                    struct pdu dpdu;
                    dpdu.type = 'D';
                    strncpy(dpdu.data, pending_download_content_name, NAME_LEN);
                    send(dl_sock, &dpdu, sizeof(char) + NAME_LEN, 0);
                    printf("Sent D-PDU to %s:%d for content '%s'.\n", pending_download_ip_str, pending_download_port, pending_download_content_name);

                    char c_type;
                    ssize_t confirmation_recv_len = recv(dl_sock, &c_type, 1, 0);
                    if (confirmation_recv_len == 1 && c_type == 'C') {
                        FILE *fp = fopen(pending_download_content_name, "wb");
                        if (!fp) {
                            perror("Error opening local file for writing");
                            continue;
                        }
                        char buffer[TCP_CHUNK_SIZE];
                        ssize_t bytes_recvd;
                        printf("Receiving content...\n");
                        while ((bytes_recvd = recv(dl_sock, buffer, TCP_CHUNK_SIZE, 0)) > 0) {
                            fwrite(buffer, 1, bytes_recvd, fp);
                        }
                        fclose(fp);
                        printf("Download of '%.10s' complete.\n", pending_download_content_name);
                    } else {
                        printf("Did not receive C-PDU from content server or invalid type. Closing connection.\n");
                        close(dl_sock);
                        is_download_socket_active = 0;
                        last_download_socket = -1;
                    }
                } // End of if (download_decision == 1)
                
            } else if (current_peer_state == WAITING_FOR_REUSE_CHOICE) {
                // User responded to "Reuse existing connection?"
                int download_decision = 0; // 0: no download, 1: new connection, 2: reuse connection

                if (strcmp(input_buffer, "yes") == 0) {
                    download_decision = 2; // Reuse existing connection
                } else {
                    download_decision = 1; // Create new connection
                }

                int dl_sock = -1;
                if (download_decision == 2) { // Reuse existing connection
                    dl_sock = last_download_socket; 
                    printf("Reusing existing connection to %s:%d.\n", pending_download_ip_str, pending_download_port);
                } else { // Create new connection
                    // Close previous connection (if not reused)
                    if (is_download_socket_active) {
                        close(last_download_socket);
                        is_download_socket_active = 0;
                        last_download_socket = -1;
                    }

                    dl_sock = socket(AF_INET, SOCK_STREAM, 0);
                    if (dl_sock < 0) {
                        perror("Error creating download TCP socket");
                        current_peer_state = NORMAL_COMMAND;
                        continue;
                    }
                    
                    if (connect(dl_sock, (struct sockaddr *)&pending_download_target_addr, sizeof(pending_download_target_addr)) < 0) {
                        perror("Error connecting to content server for download");
                        close(dl_sock);
                        current_peer_state = NORMAL_COMMAND;
                        continue;
                    }
                    printf("Created new connection to %s:%d.\n", pending_download_ip_str, pending_download_port);
                }

                // Update global tracking for potential future reuse
                last_download_socket = dl_sock;
                last_download_target_addr = pending_download_target_addr;
                is_download_socket_active = 1;
                current_peer_state = NORMAL_COMMAND; // Reset state for next command
                
                struct pdu dpdu;
                dpdu.type = 'D';
                strncpy(dpdu.data, pending_download_content_name, NAME_LEN);
                send(dl_sock, &dpdu, sizeof(char) + NAME_LEN, 0);
                printf("Sent D-PDU to %s:%d for content '%s'.\n", pending_download_ip_str, pending_download_port, pending_download_content_name);

                char c_type;
                ssize_t confirmation_recv_len = recv(dl_sock, &c_type, 1, 0);
                if (confirmation_recv_len == 1 && c_type == 'C') {
                    FILE *fp = fopen(pending_download_content_name, "wb");
                    if (!fp) {
                        perror("Error opening local file for writing");
                        continue;
                    }
                    char buffer[TCP_CHUNK_SIZE];
                    ssize_t bytes_recvd;
                    printf("Receiving content...\n");
                    while ((bytes_recvd = recv(dl_sock, buffer, TCP_CHUNK_SIZE, 0)) > 0) {
                        fwrite(buffer, 1, bytes_recvd, fp);
                    }
                    fclose(fp);
                    printf("Download of '%.10s' complete.\n", pending_download_content_name);
                } else {
                    printf("Did not receive C-PDU from content server or invalid type. Closing connection.\n");
                    close(dl_sock);
                    is_download_socket_active = 0;
                    last_download_socket = -1;
                }
            }
        } // End of FD_ISSET(STDIN_FILENO, &read_fds)


        // --- Handle incoming UDP responses from Index Server ---
        if (FD_ISSET(udp_sock, &read_fds)) {
            struct pdu rpdu_idx;
            ssize_t n_idx = recv(udp_sock, &rpdu_idx, sizeof(struct pdu), 0); // Receive PDU from server
            if (n_idx > 0) {
                 if (rpdu_idx.type == 'S') { // Search response PDU (Type 'S' | PeerName[10] | ContentName[10] | Peer_TCP_Address_Struct)
                    char found_peer_name[NAME_LEN + 1];
                    // Save info to global pending_download variables for state machine to use
                    memcpy(found_peer_name, rpdu_idx.data, NAME_LEN);
                    found_peer_name[NAME_LEN] = '\0';
                    memcpy(pending_download_content_name, rpdu_idx.data + NAME_LEN, NAME_LEN);
                    pending_download_content_name[NAME_LEN] = '\0';
                    memcpy(&pending_download_target_addr, rpdu_idx.data + (2 * NAME_LEN), sizeof(struct sockaddr_in));
                    
                    inet_ntop(AF_INET, &pending_download_target_addr.sin_addr, pending_download_ip_str, INET_ADDRSTRLEN);
                    pending_download_port = ntohs(pending_download_target_addr.sin_port);

                    // We received a valid download source.
                    // Now, set the state to wait for download choice and print the first prompt.
                    printf("\n<Server Response> Content '%.10s' found at Peer '%.10s' (%s:%d).\n", 
                           pending_download_content_name, found_peer_name, pending_download_ip_str, pending_download_port);
                    printf("Do you want to download '%.10s'? (yes/no): ", pending_download_content_name);
                    current_peer_state = WAITING_FOR_DOWNLOAD_CHOICE; // Set state to wait for download choice
                    fflush(stdout); // Ensure prompt is displayed before select is called again
                    // The main loop will now cycle, and the next STDIN_FILENO event will be handled by the state machine
                    
                 } else { // Handle A (Acknowledge), E (Error), O (Online list) responses
                    int data_len_received = n_idx - sizeof(char);
                    if (data_len_received >= 0 && data_len_received < PDU_DATA_SIZE) {
                         rpdu_idx.data[data_len_received] = '\0';
                    } else {
                         rpdu_idx.data[PDU_DATA_SIZE - 1] = '\0';
                    }
                    printf("\n<Server Response> Type: %c\n---\n%s\n---\n", rpdu_idx.type, rpdu_idx.data);
                    current_peer_state = NORMAL_COMMAND; // Ensure state is reset after any other server response
                 }
            }
        }
        
        // --- Handle incoming TCP download requests (peer acting as server) ---
        for (int i = 0; i < MAX_MY_CONTENT; i++) {
            if (my_contents[i].is_active && FD_ISSET(my_contents[i].tcp_listen_socket, &read_fds)) {
                int client_sock = accept(my_contents[i].tcp_listen_socket, NULL, NULL);
                if (client_sock >= 0) {
                    handle_tcp_download_request(client_sock, &my_contents[i]);
                } else {
                    perror("Error accepting TCP connection");
                }
            }
        }
    }

    // Cleanup before exiting.
    printf("Peer '%.10s' shutting down.\n", peer_global_name);
    close(udp_sock); // Close the UDP socket.
    if (is_download_socket_active) {
        close(last_download_socket);
    }
    return 0;
}
