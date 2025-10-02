#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>

#include "sendto_dbg.h"
#include "net_include.h"

volatile sig_atomic_t keep_running = 1;

void sig_handler(int signum) {
    if (signum == SIGINT) {
        printf("\nSIGINT received. Initiating graceful shutdown...\n");
        keep_running = 0; // Set flag to exit main loop
    }
}

static void Usage(int argc, char *argv[]);
static void Print_IP(const struct sockaddr *sa);
static void Print_help(void);
ncp_msg *circ_bbuf_get(circular_buffer *cb, int i);


/* Global configuration parameters (from command line) *///
static int Loss_rate;
static int Mode;
static char *Server_IP;
static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;

int main(int argc, char *argv[]) {
    if (signal(SIGINT, sig_handler) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    int                     sock;
    struct addrinfo         hints, *servinfo, *servaddr;
    struct sockaddr_storage from_addr;
    socklen_t               from_len;
    fd_set                  mask, read_mask;
    int                     milestone, prev_milestone_bytes, transmitted_bytes, file_bytes, bytes, num, ret;
    char                    input_buf[MAX_MESS_LEN];
    ncp_msg                 send_pkt;
    rcv_msg                 recvd_pkt;
    struct timeval          timeout;
    struct timeval          now;
    struct timeval          start;
    struct timeval          step; // Used for 10MB steps
    int                     seq, first_seq;

    /* Initialize */
    Usage(argc, argv);
    sendto_dbg_init(Loss_rate);

    /* Set up hints to use with getaddrinfo */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* we'll use AF_INET for IPv4, but can use AF_INET6 for IPv6 or AF_UNSPEC if either is ok */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    ret = getaddrinfo(Server_IP, Port_Str, &hints, &servinfo);
    if (ret != 0) {
       fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(ret));
       exit(1);
    }

    /* Loop over list of available addresses and take the first one that works
     * */
    for (servaddr = servinfo; servaddr != NULL; servaddr = servaddr->ai_next) {
        /* print IP, just for demonstration */
        printf("Found server address:\n");
        Print_IP(servaddr->ai_addr);
        printf("\n");

        /* setup socket based on addr info. manual setup would look like:
         *   socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP) */
        sock = socket(servaddr->ai_family, servaddr->ai_socktype, servaddr->ai_protocol);
        if (sock < 0) {
            perror("udp_client: socket");
            continue;
        }
        
        break; /* got a valid socket */
    }
    if (servaddr == NULL) {
        fprintf(stderr, "No valid address found...exiting\n");
        exit(1);
    }

    /* Set up mask for file descriptors we want to read from */
    FD_ZERO(&read_mask);
    FD_SET(sock, &read_mask);
    
    printf("Successfully initialized with:\n");
    printf("\tLoss rate = %d\n", Loss_rate);
    printf("\tSource filename = %s\n", Src_filename);
    printf("\tDestination filename = %s\n", Dst_filename);
    printf("\tHostname = %s\n", Hostname);
    printf("\tPort = %s\n", Port_Str);
    if (Mode == MODE_LAN) {
        printf("\tMode = LAN\n");
    } else { /*(Mode == WAN)*/
        printf("\tMode = WAN\n");
    }

    printf("\tWindow Size = %d\n", WINDOW_SIZE); //
    // Set initial sequence number, will become the last seq number in window
    seq = 0;
    first_seq = -1;
    transmitted_bytes = 0;
    prev_milestone_bytes = 0;
    file_bytes = 0;
    milestone = 1;

    // Open File to be transmitted
    FILE* file = fopen(Src_filename, "rb");
    struct stat file_stats; 
    stat(Src_filename, &file_stats);

    // Initialize Window Buffer
    circular_buffer window = {
        .head = 0,
        .seq = 0,
        .maxlen = WINDOW_SIZE
    };

    

    while(keep_running)
    {
        /* (Re)set mask */
        if (seq < first_seq + WINDOW_SIZE && first_seq != 0) {
            FD_SET((long)0, &read_mask); /* 0 == stdin */
            mask = read_mask;  
        } else {
            FD_ZERO(&read_mask);
            FD_SET(sock, &read_mask);
            mask = read_mask; 
        }
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        

        /* Wait for message (NULL timeout = wait forever) */
        num = select(FD_SETSIZE, &mask, NULL, NULL, &timeout);
        if (num > 0) {
            if (FD_ISSET(sock, &mask)) {
                from_len = sizeof(from_addr);
                bytes = recvfrom(sock, &recvd_pkt, sizeof(recvd_pkt), 0,  
                          (struct sockaddr *)&from_addr, 
                          &from_len);

                //printf("Received packet seq: %d\tflag %d\n", recvd_pkt.seq, recvd_pkt.flag);

                /* Acknowledgement Flag Handling */
                if (recvd_pkt.flag == PKT_BUSY){
                    Print_IP((struct sockaddr *)&from_addr);
                    printf("Receiver was busy.\n");
                }else if(recvd_pkt.flag == PKT_FIN) {
                    /* Print Final Statistics and exit */
                    gettimeofday(&now, NULL);
                    printf("Total time to transmit %f seconds\n", now.tv_sec - start.tv_sec + ((float) now.tv_usec - start.tv_usec)/1000000);
                    printf("File Bytes transmitted successfully: %d\n", file_bytes);
                    printf("Total Transmitted Bytes w/ retransmissions: %d\n", transmitted_bytes);
                    float throughput = (transmitted_bytes) * 8 / 1000000 / (now.tv_sec - start.tv_sec + ((float) now.tv_usec - start.tv_usec)/1000000);
                    printf("Total throughput: %f Mb/sec\n\n", throughput);
                    break;
                }else if (recvd_pkt.flag == PKT_ACK){
                    //printf("Acknowledgement received up to seq: %d\n", recvd_pkt.seq);
                    if(recvd_pkt.seq == 1 && first_seq == 0){ //
                        //first_seq = seq;
                        //printf("Acknowledgement of seq number 0 pops seq number %d\n", circ_bbuf_pop(&window)->seq);
                        circ_bbuf_pop(&window);
                        seq++;
                        first_seq++;
                    }else{ // Shift window up to revd_pkt.seq 
                        //printf("Current Seq: %d\tFirst Seq: %d\n", seq, first_seq);
                        while(first_seq < recvd_pkt.seq){ // cumulative ack handling 
                            //printf("Popped head of window and shifted. Seq: %d\n", );
                            file_bytes += circ_bbuf_pop(&window)->size;
                            if(file_bytes > milestone * MB * 10) {// check if total crossed 10MB milestone 
                                gettimeofday(&now, NULL);
                                // Print Total bytes transferred and transfer rate for 10MB step
                                printf("File Bytes transmitted successfully: %d\n", file_bytes);
                                float throughput = (transmitted_bytes-prev_milestone_bytes) * 8 / 1000000 / (now.tv_sec - step.tv_sec + ((float) now.tv_usec - step.tv_usec)/1000000);
                                printf("Throughput of last 10MB: %f Mb/sec\n\n", throughput);//

                                // Reset or advance variables
                                milestone++;
                                prev_milestone_bytes = transmitted_bytes;
                                gettimeofday(&step, NULL);
                            }
                            first_seq++;
                        }

                        // NACK handling
                        //printf("Current Seq: %d\tFirst Seq: %d\n", seq, first_seq);
                        bool *buf = recvd_pkt.buffer;
                        for(int i = 0; i < seq-first_seq; i++){
                            if(!buf[i]){ // Packet not received yet, retransmit packet from window
                                
                                send_pkt = *circ_bbuf_get(&window, i);
                                printf("Retransmitting window index %d - %d\n", i, send_pkt.seq);

                                transmitted_bytes += sizeof(send_pkt)-MAX_MESS_LEN+strlen(send_pkt.payload);
                                sendto_dbg(sock, &send_pkt,
                                    sizeof(send_pkt)-MAX_MESS_LEN+strlen(send_pkt.payload), 0,
                                    servaddr->ai_addr,
                                    servaddr->ai_addrlen);
                            }
                        }
                    }
                }
                
                //printf("Received message: %s\n", recvd_pkt.payload);

            } else if (FD_ISSET(0, &mask) && seq < first_seq + WINDOW_SIZE) { // Create and send packet when window has space (seq is less than the last sequence number in the window)           
                gettimeofday(&now, NULL);
                send_pkt.ts_sec  = now.tv_sec;
                send_pkt.ts_usec = now.tv_usec;
                if(seq == 0 && first_seq == -1){ // send start message, will only increase seq when this has been acknowledged.
                    // Mark time of beginning transmission
                    gettimeofday(&start, NULL);
                    gettimeofday(&step, NULL);
                    //printf("Start seconds %d\n", start.tv_sec );

                    send_pkt.seq = seq;
                    send_pkt.flag = PKT_START;
                    strcpy(send_pkt.payload, Dst_filename);
                    send_pkt.size = strlen(Dst_filename);

                    circ_bbuf_push(&window, &send_pkt, send_pkt.seq);
                    //printf("Start packet payload: %s", send_pkt.payload);
                    first_seq = 0;

                    transmitted_bytes += sizeof(send_pkt)-MAX_MESS_LEN+strlen(send_pkt.payload);
                    sendto_dbg(sock, &send_pkt,
                        sizeof(send_pkt)-MAX_MESS_LEN+strlen(send_pkt.payload), 0,
                        servaddr->ai_addr,
                        servaddr->ai_addrlen);
                }else if(!feof(file) && seq > 0){ // send packets while not at end of file and seq is not 0
                    /* Fill in header info */
                    send_pkt.seq = seq++;
                    send_pkt.flag = PKT_DATA;

                    /* Read and copy data into packet */
                    size_t bytes_read = fread(send_pkt.payload, sizeof(char), MAX_MESS_LEN-1, file);
                    send_pkt.payload[bytes_read] = '\0';

                    send_pkt.size = bytes_read;
                    //file_bytes += bytes_read;

                    //printf("Packet payload: %s\n", send_pkt.payload);
                    

                    circ_bbuf_push(&window, &send_pkt, send_pkt.seq-first_seq);

                    //printf("Sending packet seq: %d\n", send_pkt.seq);

                    transmitted_bytes += sizeof(send_pkt)-MAX_MESS_LEN+bytes_read;
                    sendto_dbg(sock, &send_pkt,
                        sizeof(send_pkt)-MAX_MESS_LEN+bytes_read, 0,
                        servaddr->ai_addr,
                        servaddr->ai_addrlen);
                }else if(feof(file) && seq == first_seq){ // All packets have been acknowledged, send closing packet
                    //printf("I hope I made it here first try\n");
                    send_pkt.flag = PKT_FIN;
                    send_pkt.seq = seq++;

                    circ_bbuf_push(&window, &send_pkt, send_pkt.seq-first_seq);
                    //printf("Sending closing packet w/ seq: %d\n", send_pkt.seq);

                    transmitted_bytes += sizeof(send_pkt)-MAX_MESS_LEN+strlen(send_pkt.payload);
                    sendto_dbg(sock, &send_pkt,
                        sizeof(send_pkt)-MAX_MESS_LEN+strlen(send_pkt.payload), 0,
                        servaddr->ai_addr,
                        servaddr->ai_addrlen);
                }
                
                
            }
        } else { // timeout occured, resend window//
            // printf("Timeout occured: retransmitting window\n");
            for(int i = 0; i < WINDOW_SIZE; i++){
                ncp_msg *pkt = circ_bbuf_get(&window, i);
                if(pkt != NULL){ // Packet not received yet, retransmit packet from window
                    //printf("Retransmitting seq %d\n", pkt->seq);

                    transmitted_bytes += sizeof(send_pkt)-MAX_MESS_LEN+strlen(send_pkt.payload);
                    sendto_dbg(sock, circ_bbuf_get(&window, i),
                        sizeof(send_pkt)-MAX_MESS_LEN+strlen(send_pkt.payload), 0,
                        servaddr->ai_addr,
                        servaddr->ai_addrlen);
                }
            }
        }
    }

    /* Cleanup */
    freeaddrinfo(servinfo);
    close(sock);
    fclose(file);

    /*
    for(int i = 0; i < WINDOW_SIZE; i++){
        printf("Head: %d\t", window.head);
        ncp_msg *pkt = circ_bbuf_get(&window, i);
        if(pkt != NULL){
            printf("Seq: %d\t Payload: %s\n", pkt->seq, pkt->payload);
        }
    }
        */

    return 0;

}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {
    if (argc != 5) {
        Print_help();
    }

    if (sscanf(argv[1], "%d", &Loss_rate) != 1) {
        Print_help();
    }

    if (!strncmp(argv[2], "WAN", 4)) {
        Mode = MODE_WAN;
    } else if (!strncmp(argv[2], "LAN", 4)) {
        Mode = MODE_LAN;
    } else {
        Print_help();
    }

    Src_filename = argv[3];

    // Parse the destination string: <dest_file_name>@<ip_addr>:<port>
    char *dest_str = strdup(argv[4]);  // Make a modifiable copy
    if (!dest_str) {
        perror("strdup failed");
        exit(1);
    }

    Dst_filename = strtok(dest_str, "@");
    if (Dst_filename == NULL) {
        fprintf(stderr, "Error: no destination filename before '@'\n");
        Print_help();
    }

    Server_IP = strtok(NULL, ":");
    if (Server_IP == NULL) {
        fprintf(stderr, "Error: no server IP after '@'\n");
        Print_help();
    }

    Port_Str = strtok(NULL, ":");
    if (Port_Str == NULL) {
        fprintf(stderr, "Error: no port after ':'\n");
        Print_help();
    }

    // No more tokens should be left; if there are, input is malformed
    if (strtok(NULL, ":") != NULL) {
        fprintf(stderr, "Error: too many ':' separators in destination string\n");
        Print_help();
    }
}

void Print_IP(const struct sockaddr *sa)
{
    char ipstr[INET6_ADDRSTRLEN];
    void *addr;
    char *ipver;
    uint16_t port;

    if (sa->sa_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)sa;
        addr = &(ipv4->sin_addr);
        port = ntohs(ipv4->sin_port);
        ipver = "IPv4";
    } else if (sa->sa_family == AF_INET6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)sa;
        addr = &(ipv6->sin6_addr);
        port = ntohs(ipv6->sin6_port);
        ipver = "IPv6";
    } else {
        printf("Unknown address family\n");
        return;
    }

    inet_ntop(sa->sa_family, addr, ipstr, sizeof(ipstr));
    printf("%s: %s:%d\n", ipver, ipstr, port);
}

static void Print_help(void) {
    printf("Usage: ncp <loss_rate_percent> <env> <source_file_name> <dest_file_name>@<ip_addr>:<port>\n");
    exit(0);
}


/* Circular Buffer Functions modified from https://embedjournal.com/implementing-circular-buffer-embedded-c/ */
int circ_bbuf_push(circular_buffer *cb, ncp_msg *packet, int i){
    int index = (cb->head + i) % cb->maxlen;

    // Check for buffer full (next would overwrite head)
    if (i>cb->maxlen || cb->buffer[index] != NULL) {
        return -1;  // buffer full
    }

    // Allocate memory for new packet and copy
    cb->buffer[index] = malloc(sizeof(ncp_msg));
    if (cb->buffer[index] == NULL) {
        perror("malloc failed");
        return -1;
    }
    *(cb->buffer[index]) = *packet;  // shallow copy of upkt

    return 0;
}// Modified to take an index to place data into.


/* Use to shift window */
ncp_msg *circ_bbuf_pop(circular_buffer *cb){
    if (cb->buffer[cb->head] == NULL) {
        return NULL;  // buffer empty
    }

    ncp_msg *pkt = cb->buffer[cb->head];
    cb->buffer[cb->head] = NULL;  // mark as empty
    cb->head = (cb->head + 1) % cb->maxlen;
    return pkt;
}// Modified to only pop head of circular buffer, and moving head next index.


/* Use for retransmission */
ncp_msg *circ_bbuf_get(circular_buffer *cb, int i){
    int index = (cb->head + i) % cb->maxlen;

    // Check if buffer slot is empty
    if (i>cb->maxlen || cb->buffer[index] == NULL) { // given index is outside of current window
        return NULL;  // buffer empty
    }

    // Get and return packet
    ncp_msg *pkt = cb->buffer[index];
    return pkt;
}
