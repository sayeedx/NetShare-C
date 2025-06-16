#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#include <netdb.h> // For gethostname, gethostbyname
#include <errno.h> // For errno
#include <sys/select.h> // For select()
#include <sys/stat.h> // For stat() in handle_tcp_download_request

#define SERVER_UDP_PORT 7000
#define PDU_DATA_SIZE 1500
#define TCP_CHUNK_SIZE 1399 // Same as peer.c for file transfer chunks
#define NAME_LEN 10
#define RESERVED_SERVER_NAME "IS"

#define MAX_PEERS 50
#define MAX_FILES 50

// PDU structure: Used for communication between peer and index server, and for TCP data transfer.
struct pdu {
    char type; // Type of PDU (e.g., 'R' for Register, 'S' for Search, 'O' for List, 'T' for De-register, 'A' for Acknowledge, 'E' for Error, 'D' for Download Request, 'C' for Content Chunk)
    char data[PDU_DATA_SIZE]; // Payload data, its format depends on the PDU type.
};

// Structure to hold information about content files that this peer owns and serves.
// NOTE: This struct is now used by both index_server and peer to manage local content.
struct my_content_info {
    char content_name[NAME_LEN + 1]; // Name of the content file (e.g., "file1.txt")
    char file_path[256];             // Full path to the local content file
    int tcp_listen_socket;           // TCP socket file descriptor for listening for download requests for this content
    uint16_t tcp_port_net;           // TCP port in network byte order on which this content is served
    int is_active;                   // Flag: 1 if this content is currently active and being served, 0 otherwise
};

// Global Data Structures for the Index Server:
char peer_names[MAX_PEERS][NAME_LEN + 1]; // Stores names of all registered peers
struct sockaddr_in peer_addresses[MAX_PEERS]; // Stores network addresses (IP and TCP port for downloads) of registered peers
int peer_active[MAX_PEERS]; // Tracks whether a peer entry is active (1) or inactive (0)
int peer_count = 0; // Current number of registered peer entries (including inactive ones that might be re-used)

char file_names[MAX_FILES][NAME_LEN + 1]; // Stores names of all registered files on the network
int file_count = 0; // Current number of registered files
// 2D map: file_to_peer_map[file_idx][peer_idx] = 1 if peer_idx has file_idx, 0 otherwise
int file_to_peer_map[MAX_FILES][MAX_PEERS];

// Stores the index of the last peer to register a given file.
// Used for download order prioritization.
int file_last_registered_peer_idx[MAX_FILES];

// Global: Store the actual IP address of the server.
struct in_addr server_real_ip;

// NEW for Index Server: Local content info (similar to peer.c)
struct my_content_info server_local_contents[MAX_FILES];
int server_local_content_count = 0;
int server_tcp_listen_fd = -1; // Master TCP listen socket for the server's own files
uint16_t server_tcp_port_net = 0;


// Helper function to send a PDU to a specified client address.
// sock: The UDP socket to send from.
// client_addr: The destination address of the client.
// type: The type character of the PDU.
// data: Pointer to the data payload.
// data_len: Length of the data payload.
void send_pdu(int sock, const struct sockaddr_in *client_addr, char type, const char *data, int data_len) {
    struct pdu spdu; // PDU to send
    spdu.type = type;
    // Copy data into the PDU, ensuring not to exceed PDU_DATA_SIZE.
    // data_len is crucial for correct memcpy.
    memcpy(spdu.data, data, data_len < PDU_DATA_SIZE ? data_len : PDU_DATA_SIZE);
    // Send the PDU over UDP. The size sent includes the type character + data length.
    sendto(sock, &spdu, sizeof(char) + data_len, 0, (struct sockaddr *)client_addr, sizeof(*client_addr));
}

// Finds a file's index in the global file_names array by its content name.
// Returns the index if found, -1 otherwise.
int find_file_index(const char* content_name) {
    for (int i = 0; i < file_count; i++) {
        // Compare up to NAME_LEN characters.
        if (strncmp(file_names[i], content_name, NAME_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

// Finds a peer's index in the global peer_names array by its name.
// Only considers active peers.
// Returns the index if found, -1 otherwise.
int find_peer_by_name(const char* name) {
    for (int i = 0; i < peer_count; i++) {
        // Ensure peer is active and name matches.
        if (peer_active[i] && strncmp(peer_names[i], name, NAME_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

// Finds a peer's index in the global peer_addresses array by its network address.
// Only considers active peers.
// Returns the index if found, -1 otherwise.
int find_peer_by_addr(const struct sockaddr_in* addr) {
    for (int i = 0; i < peer_count; i++) {
        // Ensure peer is active and both IP address and port match.
        // NOTE: peer_addresses stores the TCP download address provided by peers
        // for peer_idx > 0, but for IS (peer_idx == 0), it stores its own UDP bind address,
        // which will be updated for TCP if IS is to serve files.
        if (peer_active[i] &&
            peer_addresses[i].sin_addr.s_addr == addr->sin_addr.s_addr &&
            peer_addresses[i].sin_port == addr->sin_port) {
            return i;
        }
    }
    return -1;
}

// Helper to determine the server's actual local IP address.
// This function creates a dummy UDP socket and connects it to a public DNS server (e.g., Google's 8.8.8.8)
// to trigger routing. Then, getsockname is used on this socket to retrieve the local IP address
// that the system would use for outgoing connections. This helps avoid reporting 0.0.0.0.
// If it fails to determine a real IP, it defaults to the loopback address (127.0.0.1).
void get_server_local_ip(struct in_addr *ip_addr) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0); // Create a temporary UDP socket
    if (sock < 0) {
        perror("socket for IP lookup failed (using loopback)");
        ip_addr->s_addr = htonl(INADDR_LOOPBACK); // Default to loopback if socket creation fails
        return;
    }

    const char* google_dns_server = "8.8.8.8"; // A well-known public IP address
    int dns_port = 53; // Standard DNS port
    struct sockaddr_in serv; // Destination address structure for the dummy connection
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr(google_dns_server); // Set target IP
    serv.sin_port = htons(dns_port); // Set target port

    // Attempt to connect the dummy socket. This doesn't actually establish a connection
    // to the DNS server, but it forces the OS to determine the local outbound interface.
    int err = connect(sock, (const struct sockaddr*)&serv, sizeof(serv));
    if (err < 0) {
        perror("connect for IP lookup failed (using loopback)");
        close(sock);
        ip_addr->s_addr = htonl(INADDR_LOOPBACK); // Default to loopback if connect fails
        return;
    }

    struct sockaddr_in name; // Structure to hold the local address
    socklen_t namelen = sizeof(name);
    // Get the local address bound to the socket (after connect, this will be the outbound interface's IP)
    err = getsockname(sock, (struct sockaddr*)&name, &namelen);
    if (err < 0) {
        perror("getsockname for IP lookup failed (using loopback)");
        close(sock);
        ip_addr->s_addr = htonl(INADDR_LOOPBACK); // Default to loopback if getsockname fails
        return;
    }

    *ip_addr = name.sin_addr; // Store the determined local IP address
    close(sock); // Close the temporary socket
}

// NEW for Index Server: Sets up a TCP listening socket for the Index Server itself.
// This is similar to setup_tcp_listen_socket in peer.c but specific to the server's own content.
// Returns the listening socket file descriptor on success, -1 on failure.
int setup_server_tcp_listen_socket() {
    int listen_sock_fd;
    struct sockaddr_in tcp_server_addr; // Address structure for the TCP listening socket

    if ((listen_sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Error creating Index Server TCP socket");
        return -1;
    }
    
    memset(&tcp_server_addr, 0, sizeof(tcp_server_addr));
    tcp_server_addr.sin_family = AF_INET;
    tcp_server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all available interfaces
    tcp_server_addr.sin_port = htons(0); // Let the OS assign a random available port

    if (bind(listen_sock_fd, (struct sockaddr *)&tcp_server_addr, sizeof(tcp_server_addr)) < 0) {
        perror("Error binding Index Server TCP socket");
        close(listen_sock_fd);
        return -1;
    }
    if (listen(listen_sock_fd, 5) < 0) { // 5 is the backlog queue size
        perror("Error listening on Index Server TCP socket");
        close(listen_sock_fd);
        return -1;
    }

    socklen_t len = sizeof(tcp_server_addr);
    if (getsockname(listen_sock_fd, (struct sockaddr *)&tcp_server_addr, &len) == -1) {
        perror("Error getting Index Server TCP socket name (port)");
        close(listen_sock_fd);
        return -1;
    }

    server_tcp_port_net = tcp_server_addr.sin_port; // Store the obtained port
    printf("Index Server now listening for content downloads on TCP port %d\n", ntohs(server_tcp_port_net));
    return listen_sock_fd;
}

// NEW for Index Server: Handles an incoming TCP download request for server's own files.
// This is a direct copy/adaptation of handle_tcp_download_request from peer.c
// client_sock: The connected TCP socket with the requesting peer.
// serving_content: Pointer to the my_content_info of the file being requested from IS.
void handle_tcp_download_request_from_is(int client_sock, const struct my_content_info* serving_content) {
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
        printf("Index Server: Valid download request for '%.10s'. Starting transfer.\n", serving_content->content_name);
        
        // Open the requested file in binary read mode.
        fp = fopen(serving_content->file_path, "rb");
        if (!fp) {
            perror("Index Server: Error opening file for download");
            close(client_sock);
            return;
        }

        char c_pdu_type = 'C'; // 'C' PDU type to indicate content transfer is starting.
        // Send 'C' PDU type to the client to confirm readiness to send data.
        send(client_sock, &c_pdu_type, 1, 0);

        char file_buffer[TCP_CHUNK_SIZE]; // Buffer for reading file chunks
        size_t bytes_read;
        // printf("Index Server: Sending content chunks...\n"); // Too verbose
        // Read file in chunks and send them over the TCP socket.
        while ((bytes_read = fread(file_buffer, 1, TCP_CHUNK_SIZE, fp)) > 0) {
            if (send(client_sock, file_buffer, bytes_read, 0) < bytes_read) {
                perror("Index Server: Error sending file data");
                break; // Exit loop on send error
            }
        }
        fclose(fp); // Close the file after transfer.
        printf("Index Server: Transfer of '%.10s' complete.\n", serving_content->content_name);
    } else {
        fprintf(stderr, "Index Server: Invalid D-PDU received (type '%c' or wrong content name). Closing connection.\n", dpdu.type);
    }
    close(client_sock); // Close the client socket after handling the request.
}


// Initializes the server's state:
// - Clears all global data structures.
// - Sets up the Index Server itself as Peer 0 ("IS").
// - Scans the current directory for .txt files and registers them as content
//   initially available from the Index Server (Peer 0).
// server_udp_port: The port the server will be listening on, used for its own address record.
void init_server_state(int server_udp_port) {
    // Initialize all arrays to zero/empty.
    memset(peer_names, 0, sizeof(peer_names));
    memset(peer_addresses, 0, sizeof(peer_addresses));
    memset(peer_active, 0, sizeof(peer_active));
    memset(file_names, 0, sizeof(file_names));
    memset(file_to_peer_map, 0, sizeof(file_to_peer_map));
    memset(server_local_contents, 0, sizeof(server_local_contents)); // Init new local content array
    server_local_content_count = 0;
    
    // Initialize file_last_registered_peer_idx to -1 (indicating no peer has registered it yet)
    for (int i = 0; i < MAX_FILES; i++) {
        file_last_registered_peer_idx[i] = -1; 
    }

    // Determine the server's actual local IP address to use for its own content advertisement.
    get_server_local_ip(&server_real_ip);

    // Peer 0 is always the Index Server "IS".
    strcpy(peer_names[0], RESERVED_SERVER_NAME);
    peer_active[0] = 1; // Mark Index Server as active
    peer_count = 1; // Increment peer count to include the server

    // Set the server's address for peer_addresses[0] to its real IP and its *TCP* port.
    // This is because IS will now serve files, and peer_addresses stores the download address.
    // The actual port will be set after setup_server_tcp_listen_socket.
    peer_addresses[0].sin_family = AF_INET;
    peer_addresses[0].sin_addr = server_real_ip; // Use the actual determined IP
    // The sin_port for peer_addresses[0] will be updated after the TCP socket is bound.

    printf("Index Server's IP for content advertisement: %s\n", inet_ntoa(server_real_ip));

    // Scan current directory for .txt files to register them as initial content from IS.
    DIR *d;
    struct dirent *dir;
    d = opendir(".");
    if (d) {
        while ((dir = readdir(d)) != NULL && file_count < MAX_FILES) {
            // Check if the file name contains ".txt".
            if (strstr(dir->d_name, ".txt")) {
                // Copy file name, ensuring null termination and max length.
                strncpy(file_names[file_count], dir->d_name, NAME_LEN);
                file_names[file_count][NAME_LEN] = '\0';
                // Server (Peer 0) has all these files initially.
                file_to_peer_map[file_count][0] = 1;
                // Set Index Server as the last registered for its own files.
                file_last_registered_peer_idx[file_count] = 0; 

                // Store local content info for the server (needed for TCP serving)
                strncpy(server_local_contents[server_local_content_count].content_name, file_names[file_count], NAME_LEN);
                server_local_contents[server_local_content_count].content_name[NAME_LEN] = '\0';
                strncpy(server_local_contents[server_local_content_count].file_path, dir->d_name, sizeof(server_local_contents[server_local_content_count].file_path) - 1);
                server_local_contents[server_local_content_count].file_path[sizeof(server_local_contents[server_local_content_count].file_path) - 1] = '\0';
                server_local_contents[server_local_content_count].is_active = 1; // Mark as active
                server_local_content_count++;

                printf("Registered local file: %s\n", file_names[file_count]);
                file_count++; // Increment file count
            }
        }
        closedir(d); // Close the directory stream.
    }
    printf("Server initialized with %d local files. Peer 0 (%s) is the source.\n", file_count, RESERVED_SERVER_NAME);
}


int main(int argc, char *argv[]) {
    int sock; // UDP socket file descriptor
    struct sockaddr_in server_addr_udp, client_addr; // Server's and client's address structures
    struct pdu rpdu; // Received PDU
    socklen_t client_addr_len = sizeof(client_addr); // Length of client address structure
    int udp_port = SERVER_UDP_PORT; // Default UDP port

    fd_set read_fds, active_fds; // For select()
    int max_fd;

    // If an argument is provided, use it as the port number.
    if (argc > 1) udp_port = atoi(argv[1]);

    // Initialize server state (including local files and real IP detection)
    init_server_state(udp_port); 

    // Create UDP socket.
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("UDP socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Initialize server UDP address structure for binding.
    memset(&server_addr_udp, 0, sizeof(server_addr_udp));
    server_addr_udp.sin_family = AF_INET; // IPv4
    server_addr_udp.sin_addr.s_addr = INADDR_ANY; // Listen on all available network interfaces
    server_addr_udp.sin_port = htons(udp_port); // Convert port to network byte order

    // Bind the UDP socket.
    if (bind(sock, (const struct sockaddr *)&server_addr_udp, sizeof(server_addr_udp)) < 0) {
        perror("UDP bind failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // NEW: Setup Index Server's TCP listening socket for file transfers
    server_tcp_listen_fd = setup_server_tcp_listen_socket();
    if (server_tcp_listen_fd < 0) {
        fprintf(stderr, "Failed to set up Index Server's TCP listening socket. Exiting.\n");
        close(sock);
        exit(EXIT_FAILURE);
    }
    // Update peer_addresses[0] (for IS) with its actual TCP port for downloads
    peer_addresses[0].sin_port = server_tcp_port_net;


    printf("Index Server '%s' listening on UDP port %d, serving content on TCP port %d\n", 
           RESERVED_SERVER_NAME, udp_port, ntohs(server_tcp_port_net));

    // Initialize select() sets
    FD_ZERO(&active_fds);
    FD_SET(sock, &active_fds); // Add UDP socket
    FD_SET(server_tcp_listen_fd, &active_fds); // Add TCP listening socket
    max_fd = (sock > server_tcp_listen_fd) ? sock : server_tcp_listen_fd;


    // Main server loop: Continuously receive and process PDUs and TCP connections.
    while (1) {
        read_fds = active_fds; // Copy active_fds for each select() call
        
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue; // Handle interrupted system call
            perror("select failed");
            exit(EXIT_FAILURE);
        }

        // --- Handle UDP messages (Control Plane) ---
        if (FD_ISSET(sock, &read_fds)) {
            int n = recvfrom(sock, &rpdu, sizeof(struct pdu), 0, (struct sockaddr *)&client_addr, &client_addr_len);
            if (n <= 0) continue; 

            char client_ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip_str, INET_ADDRSTRLEN);
            printf("\nReceived PDU type '%c' from %s:%d\n", rpdu.type, client_ip_str, ntohs(client_addr.sin_port));

            switch (rpdu.type) {
                case 'R': { // Register content PDU (Type 'R' | PeerName[10] | ContentName[10] | TCP_Address_Struct)
                    char peer_name[NAME_LEN + 1];
                    char content_name[NAME_LEN + 1];
                    struct sockaddr_in peer_tcp_addr; // This is the TCP address the peer will listen on for downloads

                    memcpy(peer_name, rpdu.data, NAME_LEN);
                    peer_name[NAME_LEN] = '\0';
                    memcpy(content_name, rpdu.data + NAME_LEN, NAME_LEN);
                    content_name[NAME_LEN] = '\0';
                    memcpy(&peer_tcp_addr, rpdu.data + (2 * NAME_LEN), sizeof(struct sockaddr_in));

                    printf("Register request: Peer '%s', Content '%s', Client UDP: %s:%d, Peer TCP: %s:%d\n",
                           peer_name, content_name,
                           client_ip_str, ntohs(client_addr.sin_port),
                           inet_ntoa(peer_tcp_addr.sin_addr), ntohs(peer_tcp_addr.sin_port));

                    int file_idx = find_file_index(content_name);
                    if (file_idx == -1) {
                        if (file_count >= MAX_FILES) {
                             send_pdu(sock, &client_addr, 'E', "Server full: Cannot register new content.", strlen("Server full: Cannot register new content.") + 1);
                             continue;
                        }
                        file_idx = file_count++;
                        strncpy(file_names[file_idx], content_name, NAME_LEN);
                        file_names[file_idx][NAME_LEN] = '\0';
                        printf("New file registered: %s at index %d\n", file_names[file_idx], file_idx);
                    }

                    int name_idx = find_peer_by_name(peer_name);
                    int addr_idx = find_peer_by_addr(&client_addr);

                    if ((name_idx != -1 && addr_idx != -1 && name_idx != addr_idx) ||
                        (name_idx != -1 && addr_idx == -1 && peer_addresses[name_idx].sin_addr.s_addr != client_addr.sin_addr.s_addr) ||
                        (name_idx == -1 && addr_idx != -1 && strncmp(peer_names[addr_idx], peer_name, NAME_LEN) != 0)) {
                        
                        send_pdu(sock, &client_addr, 'E', "Peer name or address conflict.", strlen("Peer name or address conflict.") + 1);
                        continue;
                    }
                    
                    int peer_idx = -1;
                    if (name_idx != -1) {
                        peer_idx = name_idx;
                        peer_addresses[peer_idx].sin_addr = client_addr.sin_addr;
                        // peer_addresses[peer_idx].sin_port = client_addr.sin_port; // Keep TCP port
                    } else if (addr_idx != -1) {
                        peer_idx = addr_idx;
                        strncpy(peer_names[peer_idx], peer_name, NAME_LEN);
                        peer_names[peer_idx][NAME_LEN] = '\0';
                    } else {
                        if (peer_count >= MAX_PEERS) {
                            send_pdu(sock, &client_addr, 'E', "Server full: Cannot register new peer.", strlen("Server full: Cannot register new peer.") + 1);
                            continue;
                        }
                        peer_idx = peer_count++;
                        strncpy(peer_names[peer_idx], peer_name, NAME_LEN);
                        peer_names[peer_idx][NAME_LEN] = '\0';
                        // Initially store the UDP address, will be overwritten by TCP address
                        peer_addresses[peer_idx].sin_addr = client_addr.sin_addr; 
                        peer_addresses[peer_idx].sin_port = client_addr.sin_port; // This is peer's UDP port
                        peer_active[peer_idx] = 1;
                        printf("New peer registered: %s at index %d\n", peer_names[peer_idx], peer_idx);
                    }

                    if (file_to_peer_map[file_idx][peer_idx] == 1) {
                        send_pdu(sock, &client_addr, 'E', "Content already registered by this peer.", strlen("Content already registered by this peer.") + 1);
                        continue;
                    }
                    
                    file_to_peer_map[file_idx][peer_idx] = 1;
                    // OVERWRITE WITH THE PEER'S TCP LISTENING ADDRESS FOR DOWNLOADS
                    peer_addresses[peer_idx] = peer_tcp_addr; 
                    peer_active[peer_idx] = 1;

                    file_last_registered_peer_idx[file_idx] = peer_idx;

                    char success_msg[100];
                    snprintf(success_msg, sizeof(success_msg), "File '%.10s' has been successfully registered by '%.10s'", content_name, peer_name);
                    send_pdu(sock, &client_addr, 'A', success_msg, strlen(success_msg) + 1);
                    printf("Registered content '%s' for peer '%s'.\n", content_name, peer_name);
                    break;
                }
                case 'T': { // De-register content PDU (Type 'T' | PeerName[10] | ContentName[10])
                    char peer_name[NAME_LEN + 1];
                    char content_name[NAME_LEN + 1];
                    
                    memcpy(peer_name, rpdu.data, NAME_LEN);
                    peer_name[NAME_LEN] = '\0';
                    memcpy(content_name, rpdu.data + NAME_LEN, NAME_LEN);
                    content_name[NAME_LEN] = '\0';

                    printf("De-register request: Peer '%s', Content '%s'\n", peer_name, content_name);

                    int peer_idx = find_peer_by_name(peer_name);
                    int file_idx = find_file_index(content_name);

                    // For de-registration, client_addr must match the *original UDP address* from registration
                    // to prevent spoofing. We need a way to store/retrieve the UDP address.
                    // Current peer_addresses stores TCP download address after registration.
                    // This check needs to be based on the UDP source that sent the 'T' PDU.
                    // For now, let's simplify and check if the peer exists by name and is active.
                    // A more robust solution might require a separate peer_udp_addresses array.
                    
                    // Simple check for now: does the peer exist by name and is it active?
                    if (peer_idx == -1 || !peer_active[peer_idx]) {
                        send_pdu(sock, &client_addr, 'E', "Peer not found or inactive.", strlen("Peer not found or inactive.") + 1);
                        continue;
                    }

                    if (file_idx == -1 || file_to_peer_map[file_idx][peer_idx] == 0) {
                        char err_msg[100];
                        snprintf(err_msg, sizeof(err_msg), "File '%.10s' is not registered to peer '%.10s'", content_name, peer_name);
                        send_pdu(sock, &client_addr, 'E', err_msg, strlen(err_msg) + 1);
                        continue;
                    }
                    
                    file_to_peer_map[file_idx][peer_idx] = 0;

                    if (file_last_registered_peer_idx[file_idx] == peer_idx) {
                        file_last_registered_peer_idx[file_idx] = -1;
                    }
                    
                    int has_other_files = 0;
                    for (int i = 0; i < file_count; i++) {
                        if (file_to_peer_map[i][peer_idx] == 1) {
                            has_other_files = 1;
                            break;
                        }
                    }
                    
                    if (!has_other_files && peer_idx != 0) {
                        peer_active[peer_idx] = 0;
                        printf("Peer '%.10s' has no more files. Deactivating entry.\n", peer_name);
                    }
                    
                    char success_msg[100];
                    snprintf(success_msg, sizeof(success_msg), "File '%.10s' has been successfully deregistered by '%.10s'", content_name, peer_name);
                    send_pdu(sock, &client_addr, 'A', success_msg, strlen(success_msg) + 1);
                    printf("De-registered content '%s' for peer '%s'.\n", content_name, peer_name);
                    break;
                }
                case 'S': { // Search for content PDU (Type 'S' | ContentName[10])
                    char search_content_name[NAME_LEN + 1];
                    memcpy(search_content_name, rpdu.data, NAME_LEN);
                    search_content_name[NAME_LEN] = '\0';

                    printf("Search request for content: '%s'\n", search_content_name);

                    int file_idx = find_file_index(search_content_name);
                    if (file_idx == -1) {
                        send_pdu(sock, &client_addr, 'E', "Content not found.", sizeof("Content not found."));
                        break;
                    }

                    int found_source_peer_idx = -1;

                    // Prioritize download sources:
                    // 1. Try the last registered peer
                    int last_reg_peer_idx = file_last_registered_peer_idx[file_idx];
                    if (last_reg_peer_idx != -1 && peer_active[last_reg_peer_idx] && file_to_peer_map[file_idx][last_reg_peer_idx] == 1) {
                        found_source_peer_idx = last_reg_peer_idx;
                        printf("Priority 1: Found '%s' at last registered Peer '%.10s'.\n", file_names[file_idx], peer_names[found_source_peer_idx]);
                    } else {
                        // 2. If last registered peer is not available/active, try any other active peer
                        printf("Last registered peer for '%s' not available or active. Searching other peers.\n", file_names[file_idx]);
                        for (int i = 1; i < peer_count; i++) { // Start from 1 to skip 'IS'
                            if (peer_active[i] && file_to_peer_map[file_idx][i] == 1) {
                                found_source_peer_idx = i;
                                printf("Priority 2: Found '%s' at Peer '%.10s'.\n", file_names[file_idx], peer_names[found_source_peer_idx]);
                                break;
                            }
                        }

                        // 3. If no other active peers, try the Index Server (Peer 0) itself
                        if (found_source_peer_idx == -1 && peer_active[0] && file_to_peer_map[file_idx][0] == 1) {
                            found_source_peer_idx = 0; // Index Server (Peer 0)
                            printf("Priority 3: No peers found, falling back to Index Server for '%s'.\n", file_names[file_idx]);
                        }
                    }

                    // Send the 'S' PDU response if a valid source was found.
                    if (found_source_peer_idx != -1) {
                        char s_response_data[PDU_DATA_SIZE];
                        struct sockaddr_in source_addr_for_download;

                        if (found_source_peer_idx == 0) { // If source is IS itself
                            // Use server_real_ip and server_tcp_port_net for IS download address
                            source_addr_for_download.sin_family = AF_INET;
                            source_addr_for_download.sin_addr = server_real_ip;
                            source_addr_for_download.sin_port = server_tcp_port_net;
                        } else {
                            // For other peers, use the TCP address stored in peer_addresses
                            source_addr_for_download = peer_addresses[found_source_peer_idx];
                        }

                        memcpy(s_response_data, peer_names[found_source_peer_idx], NAME_LEN);
                        memcpy(s_response_data + NAME_LEN, file_names[file_idx], NAME_LEN);
                        memcpy(s_response_data + 2 * NAME_LEN, &source_addr_for_download, sizeof(struct sockaddr_in));
                        
                        printf("Sending source info for '%s' to client: Peer '%.10s' (%s:%d).\n", 
                               file_names[file_idx], peer_names[found_source_peer_idx], 
                               inet_ntoa(source_addr_for_download.sin_addr), ntohs(source_addr_for_download.sin_port));
                        send_pdu(sock, &client_addr, 'S', s_response_data, 2 * NAME_LEN + sizeof(struct sockaddr_in));
                    } else {
                        send_pdu(sock, &client_addr, 'E', "Content exists but no active sources online.", sizeof("Content exists but no active sources online."));
                        printf("Content '%s' exists but no active source found after prioritization.\n", search_content_name);
                    }
                    break;
                }
                case 'O': { // List all online content PDU (Type 'O' | no data)
                    char list_data[PDU_DATA_SIZE] = {0};
                    int current_len = 0;

                    printf("List content request received.\n");

                    for (int i = 0; i < file_count; i++) {
                        current_len += snprintf(list_data + current_len, PDU_DATA_SIZE - current_len, "%.10s |", file_names[i]);
                        
                        for (int j = 0; j < peer_count; j++) {
                            if (peer_active[j] && file_to_peer_map[i][j] == 1) {
                                current_len += snprintf(list_data + current_len, PDU_DATA_SIZE - current_len, " %.10s", peer_names[j]);
                            }
                        }
                        current_len += snprintf(list_data + current_len, PDU_DATA_SIZE - current_len, "\n");
                    }
                    printf("Sending content list to requester.\n");
                    send_pdu(sock, &client_addr, 'O', list_data, strlen(list_data) + 1);
                    break;
                }
                default:
                    send_pdu(sock, &client_addr, 'E', "Unknown PDU.", sizeof("Unknown PDU."));
                    printf("Received unknown PDU type: %c\n", rpdu.type);
            }
        }

        // --- Handle TCP connections (Data Plane for Index Server's own files) ---
        if (FD_ISSET(server_tcp_listen_fd, &read_fds)) {
            int client_sock = accept(server_tcp_listen_fd, NULL, NULL);
            if (client_sock >= 0) {
                // Find which local file the client is requesting.
                // For simplicity, we assume the D-PDU will specify it.
                // We'll pass a dummy content info and let handle_tcp_download_request_from_is figure it out from D-PDU.
                // A more robust server might map client_sock to a specific file request state.
                
                // Read the D-PDU to determine which file is requested
                struct pdu dpdu_check;
                ssize_t n_check = recv(client_sock, &dpdu_check, sizeof(dpdu_check), MSG_PEEK); // Peek to not consume
                if (n_check > 0 && dpdu_check.type == 'D') {
                    char requested_content_name[NAME_LEN + 1];
                    strncpy(requested_content_name, dpdu_check.data, NAME_LEN);
                    requested_content_name[NAME_LEN] = '\0';

                    // Find the local content entry for the requested file
                    struct my_content_info *serving_content = NULL;
                    for (int i = 0; i < server_local_content_count; i++) {
                        if (server_local_contents[i].is_active && strncmp(server_local_contents[i].content_name, requested_content_name, NAME_LEN) == 0) {
                            serving_content = &server_local_contents[i];
                            break;
                        }
                    }

                    if (serving_content) {
                        handle_tcp_download_request_from_is(client_sock, serving_content);
                    } else {
                        fprintf(stderr, "Index Server: Download request for unknown or inactive local content '%s'. Closing connection.\n", requested_content_name);
                        close(client_sock);
                    }
                } else {
                    fprintf(stderr, "Index Server: Invalid or empty initial TCP data. Closing connection.\n");
                    close(client_sock);
                }
            } else {
                perror("Index Server: Error accepting TCP connection");
            }
        }
    }
    close(sock); // Close the UDP socket.
    close(server_tcp_listen_fd); // Close the master TCP listen socket.
    return 0;
}
