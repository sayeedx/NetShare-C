# P2PC-Share: A C-based Peer-to-Peer File Sharing System

## Overview

P2PC-Share is a command-line peer-to-peer (P2P) file sharing application built in C. It facilitates the discovery and transfer of content (files) between multiple peers through a central Index Server. The system is designed to demonstrate fundamental concepts of distributed systems, including client-server communication (between peers and the Index Server) and direct peer-to-peer communication (for file transfers).

## Features

* **Content Registration:** Peers can register local files they wish to share with the Index Server.

* **Content Search:** Peers can query the Index Server to find available content on the network.

* **Online Content Listing:** Peers can request a list of all content currently registered with the Index Server, along with the peers hosting them.

* **Direct Peer-to-Peer Download:** Once content is discovered, peers can initiate direct TCP connections to serving peers (or the Index Server itself) to download files.

* **Download Source Prioritization:** The Index Server prioritizes download sources, favoring the last peer that registered a file, then other active peers, and finally itself (if configured to serve).

* **Download Connection Reuse (Peer Client):** The downloading peer client offers an option to reuse an existing TCP connection to a serving peer if subsequent downloads are from the same source.

* **Content Deregistration:** Peers can de-register content they no longer wish to share.

* **Index Server as Content Source:** The Index Server can also host and serve its own local files, acting as a peer for direct downloads.

* **Robust Input Handling (Peer):** Improved command-line input and prompt management for a smoother user experience.

## Architecture

The system consists of two main components:

1.  **Index Server (`index_server.c`):**

    * Acts as a central directory for available content and active peers.

    * Handles `Register (R)`, `De-register (T)`, `Search (S)`, and `List (O)` PDUs (Protocol Data Units) via UDP.

    * Maintains a map of files to peers that possess them.

    * **New in Sprint 6:** Can also serve its own local files directly via TCP, functioning as a regular peer for content delivery.

2.  **Peer Application (`peer.c`):**

    * Represents individual nodes in the P2P network.

    * Registers its local content with the Index Server.

    * Acts as a client to search for and download content from other peers.

    * Acts as a server to serve its own registered content to other peers via TCP.

    * Communicates with the Index Server via UDP and with other peers via TCP.

## Prerequisites

To compile and run this project, you will need:

* A C compiler (e.g., GCC or Clang)

* `make` (optional, but recommended for build automation)

* A Linux/Unix-like environment (or WSL on Windows, or MinGW for native Windows compilation).

## Compilation

Navigate to the project directory in your terminal and compile both components:

```bash
gcc index_server.c -o index_server -Wall -Wextra
gcc peer.c -o peer -Wall -Wextra
```

The `-Wall` and `-Wextra` flags enable useful compiler warnings, which are highly recommended during development.

## Usage

You need to run the Index Server first, and then one or more Peer applications.

### 1. Start the Index Server

The Index Server needs to be running before any peers connect. It can be started with an optional UDP port (default is `7000`). It will automatically scan its directory for `.txt` files and register them as its own content. It will also print the TCP port it's listening on for its own content downloads.

```bash
./index_server [UDP_PORT]
```

**Example:**

```bash
./index_server 14000
```

*(The server will print its real IP for advertisement and the TCP port it's listening on for downloads)*

### 2. Start Peer Applications

Each peer application requires a unique name, the Index Server's IP address, and its UDP port.

```bash
./peer <YourPeerName> [IndexServerIP] [IndexServerUDPPort]
```

**Example:**

```bash
./peer PeerA 127.0.0.1 14000
./peer PeerB 127.0.0.1 14000
```

**Important:** Each peer instance MUST have a unique `<YourPeerName>`. Running multiple peers with the same name will lead to conflicts and undefined behavior on the Index Server.

### Peer Commands

Once a peer is running, you can interact with it using the following commands:

* `register <content_name> <file_path>`: Registers a local file to be shared with the network via the Index Server.

    * `content_name`: The name by which the file will be known in the network (max 10 chars).

    * `file_path`: The actual path to the file on your local system.

    * **Example:** `register mydoc.txt /home/user/documents/my_document.txt`

* `search <content_name>`: Searches for a specific content file on the network via the Index Server. If found, it will prompt you to download.

    * **Example:** `search mydoc.txt`

* `list`: Requests a list of all active content and the peers currently sharing them from the Index Server.

    * **Example:** `list`

* `deregister <content_name>`: De-registers a previously registered content file.

    * **Example:** `deregister mydoc.txt`

* `quit`: Shuts down the peer, de-registering all its active content from the Index Server.

    * **Example:** `quit`

### Example Workflow:

1.  **Start Server:**

    ```bash
    ./index_server 7000
    # Output: Index Server ... listening on UDP port 7000, serving content on TCP port XXXXX
    ```

    *(Ensure `test.txt` and `sample.txt` exist in the server's directory)*

2.  **Start Peer A:**

    ```bash
    ./peer PeerA 127.0.0.1 7000
    ```

3.  **Peer A registers a file:**

    ```
    PeerA> register mynovel.txt /path/to/my/novel.txt
    ```

4.  **Start Peer B:**

    ```bash
    ./peer PeerB 127.0.0.1 7000
    ```

5.  **Peer B lists content:**

    ```
    PeerB> list
    # Output:
    # <Server Response> Type: O
    # ---
    # test.txt | IS
    # sample.txt | IS
    # mynovel.txt | PeerA
    # ---
    ```

6.  **Peer B searches for content and downloads from Peer A:**

    ```
    PeerB> search mynovel.txt
    # Output:
    # <Server Response> Content 'mynovel.txt' found at Peer 'PeerA' (127.0.0.1:YYYYY).
    # Do you want to download 'mynovel.txt'? (yes/no): yes
    # (If multiple downloads from PeerA: Reuse existing connection to 127.0.0.1:YYYYY? (yes/no): yes)
    # Created new connection to 127.0.0.1:YYYYY.
    # Sent D-PDU to 127.0.0.1:YYYYY for content 'mynovel.txt'.
    # Receiving content...
    # Download of 'mynovel.txt' complete.
    ```

    *(A new file `mynovel.txt` will appear in Peer B's directory)*

7.  **Peer B searches for content and downloads from Index Server:**

    ```
    PeerB> search test.txt
    # Output:
    # <Server Response> Content 'test.txt' found at Peer 'IS' (127.0.0.1:XXXXX).
    # Do you want to download 'test.txt'? (yes/no): yes
    # Created new connection to 127.0.0.1:XXXXX.
    # Sent D-PDU to 127.0.0.1:XXXXX for content 'test.txt'.
    # Receiving content...
    # Download of 'test.txt' complete.
    ```

    *(A new file `test.txt` will appear in Peer B's directory)*

## Known Issues and Limitations

* **Firewalls:** Ensure no firewalls are blocking UDP (default 7000) or dynamic TCP ports between your machines, as this is a common cause of "Connection refused" errors.

* **Unique Peer Names:** As mentioned, each peer instance must have a unique name. The system does not prevent duplicate names from being launched, which can lead to server-side confusion.

* **Error Handling:** Basic error handling is in place, but robust error recovery for all network conditions (e.g., disconnected peers during download) is limited.

* **File Overwriting:** Downloads will overwrite existing files with the same name in the peer's current directory without a prompt.

* **Simple PDU Structure:** The PDU size is fixed, which limits metadata exchange.

* **No Concurrent Downloads:** The peer client handles downloads sequentially. The "reuse connection" feature is for sequential downloads from the *same* source.

* **Single-Threaded I/O for Server/Peer:** Both components use `select()` for non-blocking I/O but process requests sequentially within their main loops. Heavy load might cause delays.

## Future Enhancements

* **Multi-threading/Asynchronous I/O:** Improve performance by handling multiple concurrent TCP connections (downloads/uploads) using threads or a more advanced asynchronous I/O model.

* **Robust Connection Management:** Implement keep-alives, more sophisticated timeout handling, and automatic re-connection attempts.

* **GUI Interface:** Develop a graphical user interface for easier interaction.

* **Dynamic Port Management:** Allow peers to explicitly choose their TCP listening ports.

* **Download Progress Indicator:** Display download progress to the user.

* **Hash Verification:** Implement content integrity checks (e.g., MD5/SHA checksums) to ensure downloaded files are not corrupted.

* **Peer Discovery (Decentralized Index):** Explore options for decentralized peer discovery, reducing reliance on a single Index Server.

* **Advanced Search:** Implement keyword searching or more complex queries.

* **Transfer Resumption:** Allow interrupted downloads to resume from where they left off.
