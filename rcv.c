#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static void Print_help(void);
static int Cmp_time(struct timeval t1, struct timeval t2);

void circ_bbuf_shift(circular_buffer *cb, FILE* file);


/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;

u_int32_t               expected_seq, last_seq;
int                     written_bytes;

static const struct timeval Zero_time = {0, 0};


int main(int argc, char *argv[]) {
    if (signal(SIGINT, sig_handler) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    struct addrinfo         hints, *servinfo, *servaddr;
    struct sockaddr_storage from_addr, session_addr;
    socklen_t               from_len, session_len;
    int                     sock;
    fd_set                  mask, read_mask;
    int                     milestone, prev_milestone_bytes, received_bytes, bytes, num, ret, close_retransmissions;
    // char                    mess_buf[MAX_MESS_LEN];
    struct timeval          timeout;
    struct timeval          last_recv_time = {0, 0};
    struct timeval          now;
    struct timeval          diff_time;
    struct timeval          temp;
    struct timeval          step;
    struct timeval          start;
    ncp_msg                 recvd_pkt;  // NOTE: will need to change to ncp_packet at somepoint
    char                    hbuf[NI_MAXHOST], sbuf[NI_MAXSERV], session_hbuf[NI_MAXHOST], session_sbuf[NI_MAXSERV];
    bool                    active_session;

    // NOTE: Dst_filename will eventually not be hard coded, and will be read in from the start message.
    //static char *Dst_filename = "output.txt";
    FILE* file;
    char* Dst_filename;
    

    /* Initialize */
    Usage(argc, argv);
    sendto_dbg_init(Loss_rate);

    /* Set up hints */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* we'll use AF_INET for IPv4, but can use AF_INET6 for IPv6 or AF_UNSPEC if either is ok */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE; /* indicates that I want to get my own IP address */

    /* Initialize Buffer */
    circular_buffer buf = {
            .head = 0,
            .seq = 0,
            .maxlen = WINDOW_SIZE
        };

    ret = getaddrinfo(NULL, Port_Str, &hints, &servinfo);
    if (ret != 0) {
       fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(ret));
       exit(1);
    }

    /* Select IP Address and bind */
    for (servaddr = servinfo; servaddr != NULL; servaddr = servaddr->ai_next) {
        /* print IP, just for demonstration */
        ret = getnameinfo(servaddr->ai_addr, servaddr->ai_addrlen, hbuf,
                sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST |
                NI_NUMERICSERV);
        if (ret != 0) {
            fprintf(stderr, "getnameinfo error: %s\n", gai_strerror(ret));
            exit(1);
        }
        

        /* setup socket based on addr info. manual setup would look like:
         *   socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP) */
        sock = socket(servaddr->ai_family, servaddr->ai_socktype, servaddr->ai_protocol);
        if (sock < 0) {
            perror("udp_server: socket");
            continue;
        }

        /* bind to receive incoming messages on this socket */
        if (bind(sock, servaddr->ai_addr, servaddr->ai_addrlen) < 0) {
            close(sock);
            perror("udp_server: bind");
            continue;
        }
        
        break; /* got a valid socket */
    }

    if (servaddr == NULL) {
        fprintf(stderr, "No valid address found...exiting\n");
        exit(1);
    }

    printf("Successfully initialized with:\n");
    printf("\tLoss rate = %d\n", Loss_rate);
    printf("\tPort = %s\n", Port_Str);
    if (Mode == MODE_LAN) {
        printf("\tMode = LAN\n");
    } else { /*(Mode == WAN)*/
        printf("\tMode = WAN\n");
    }

    printf("Listening on IP address: %s:%s\n\n", hbuf, sbuf);

    /* Hard Coded seq number start, will be filled by starting message of transmission. *///
    expected_seq = 0;
    close_retransmissions = -1;
    received_bytes = 0;
    prev_milestone_bytes = 0;
    written_bytes = 0;
    milestone = 1;

    /* Set up mask for file descriptors we want to read from */
    FD_ZERO(&read_mask);
    FD_SET(sock, &read_mask);

    /* While active session is false receiver will look for new requests */
    active_session = false;

    while(keep_running)
    {
        /* (Re)set mask and timeout */
        mask = read_mask;
        timeout.tv_sec = 0;
        timeout.tv_usec = 200; // LAN - 200, WAN - 40000

        /* Wait for message or timeout */
        num = select(FD_SETSIZE, &mask, NULL, NULL, &timeout);
        if (num > 0) {
            if (FD_ISSET(sock, &mask)) {
                from_len = sizeof(from_addr);
                bytes = recvfrom(sock, &recvd_pkt, sizeof(recvd_pkt), 0,  
                          (struct sockaddr *)&from_addr, 
                          &from_len);

                /* Record time we received this msg */
                gettimeofday(&last_recv_time, NULL);

                /* print IP received from */
                ret = getnameinfo((struct sockaddr *)&from_addr, from_len, hbuf,
                        sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST |
                        NI_NUMERICSERV);
                if (ret != 0) {
                    fprintf(stderr, "getnameinfo error: %s\n", gai_strerror(ret));
                    exit(1);
                }

                if(recvd_pkt.flag == PKT_START){
                    /* Establish active session on start packet */
                    rcv_msg ack_pkt = {
                        .seq = recvd_pkt.seq+1,
                        .buffer = NULL
                    };
                    recvd_pkt.size = bytes - (sizeof(recvd_pkt) - MAX_MESS_LEN);
                    //printf("Start packet payload: %s\n", recvd_pkt.payload);

                    if (!active_session /*&& recvd_pkt.flag == PKT_START*/){
                        strcpy(session_hbuf, hbuf);
                        strcpy(session_sbuf, sbuf);
                        memcpy(&session_addr, &from_addr, sizeof(struct sockaddr_storage));
                        session_len = from_len;

                        expected_seq = recvd_pkt.seq+1;
                        active_session = true;
                        printf("Establishing session with %s:%s\n", session_hbuf, session_sbuf);

                        gettimeofday(&start, NULL);
                        gettimeofday(&step, NULL);

                        ack_pkt.flag = PKT_ACK;

                        Dst_filename = malloc(recvd_pkt.size + 1);
                        if (!Dst_filename) {
                            perror("malloc failed");
                            exit(1);
                        }
                        memcpy(Dst_filename, recvd_pkt.payload, recvd_pkt.size);
                        Dst_filename[recvd_pkt.size] = '\0';

                        file = fopen(Dst_filename, "wb");
                        if (!file) {
                            perror("fopen");
                            exit(EXIT_FAILURE);
                        }
                    }else{
                        //printf("Dropped packet. %s:%s is not the active session client.\n", hbuf, sbuf);
                        ack_pkt.flag = PKT_BUSY;
                    }
                    sendto_dbg(sock, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&from_addr,
                        from_len);
                }else if (recvd_pkt.flag == PKT_FIN) {
                    // Build response packet acknowledging the closing packet
                    // It will be set to resend a minimum amount of time before the receiver times out and enters listening mode
                    gettimeofday(&now, NULL);
                    printf("Total time to transmit %f seconds\n", now.tv_sec - start.tv_sec + ((float) now.tv_usec - start.tv_usec)/1000000);
                    printf("File Bytes transmitted successfully: %d\n", written_bytes);
                    float throughput = (written_bytes) * 8 / 1000000 / (now.tv_sec - start.tv_sec + ((float) now.tv_usec - start.tv_usec)/1000000);
                    printf("Total throughput: %f Mb/sec\n\n", throughput);

                    expected_seq = recvd_pkt.seq;
                    rcv_msg ack_pkt = {
                        .seq = expected_seq,
                        .buffer = NULL,
                        .flag = PKT_FIN
                    };
                    

                    sendto_dbg(sock, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&from_addr,
                        from_len);

                    if (strcmp(session_hbuf, hbuf) == 0 && strcmp(session_sbuf, sbuf) == 0){ // Only enters ensured retransmission for current session client
                        timeout.tv_sec = 10;
                        close_retransmissions = 0; // Will keep track of number of retransmission of closing acknowledgement and signal timeouts to use this.
                    }
                }else if (recvd_pkt.flag == PKT_DATA) {
                    /* Process data - write to file or store in buffer */
                    if(strcmp(session_hbuf, hbuf) == 0 && strcmp(session_sbuf, sbuf) == 0){
                        if(recvd_pkt.seq > (expected_seq + WINDOW_SIZE - 1) || recvd_pkt.seq < expected_seq){
                            //printf("Dropped packet seq %d outside of current window.\n", recvd_pkt.seq);
                        }else if(recvd_pkt.seq == expected_seq){
                            // Deliver to application and progress expected seq
                            fwrite(recvd_pkt.payload, 1, strlen(recvd_pkt.payload), file);
                            written_bytes += strlen(recvd_pkt.payload);
                            if(written_bytes > milestone * MB * 10) {// check if total crossed 10MB milestone 
                                gettimeofday(&now, NULL);
                                // Print Total bytes transferred and transfer rate for 10MB step
                                printf("File bytes written successfully: %d\n", written_bytes);
                                float throughput = (written_bytes-prev_milestone_bytes) * 8 / 1000000 / (now.tv_sec - step.tv_sec + ((float) now.tv_usec - step.tv_usec)/1000000);
                                printf("Throughput of last 10MB: %f Mb/sec\n\n", throughput);//

                                // Reset or advance variables
                                milestone++;
                                prev_milestone_bytes = written_bytes;
                                gettimeofday(&step, NULL);
                            }


                            expected_seq++;
                            circ_bbuf_shift(&buf, file); // Shift buffer until new pointer is NULL, writing any packets in the process
                        }else{
                            // Store in buf at corresponding position
                            
                            if(circ_bbuf_push(&buf, &recvd_pkt, (recvd_pkt.seq - expected_seq)) == 0){
                                //printf("Stored seq: %d in buffer\n", recvd_pkt.seq);
                            }
                        }
                    }else{
                        // Sender is not current client, send busy acknowledgement
                        printf("Dropped packet from %s:%s. Not active session client.\n", hbuf, sbuf);
                        
                        /* Do I really need to respond to non-startup attempts?
                        rcv_msg ack_pkt = {
                            .flag = PKT_BUSY,
                            .seq = recvd_pkt.seq,
                            .buffer = NULL
                        };//
                        printf("Size of acknowledgement: %lu", sizeof(ack_pkt));
                        sendto_dbg(sock, &ack_pkt, bytes, 0, (struct sockaddr *)&from_addr,
                        from_len);
                        */
                    }
                }
            }
        } else { // timeout occured
            /* Send Cumulative Acknowledgement and NACK packet when in active session */
            if(active_session){
                //printf("Close Retransmission Count: %d\n", close_retransmissions);
                if (close_retransmissions >=0){
                    rcv_msg ack_pkt = {
                        .seq = expected_seq,
                        .buffer = NULL,
                        .flag = PKT_FIN
                    };
                    
                    //printf("Ack_pkt flag value: %d\n", ack_pkt.flag);
                    
                    sendto_dbg(sock, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&session_addr,
                           session_len);

                    close_retransmissions++;
                    //printf("Again Close Retransmission Count: %d\n", close_retransmissions);
                    if(close_retransmissions >= MAX_RETRANSMISSIONS){ 
                        printf("Closing file and returning to listening.\n");
                        active_session = false;
                        expected_seq = 0;
                        close_retransmissions = -1;
                        fclose(file);
                        for(int i = 0; i < WINDOW_SIZE; i++){
                            ncp_msg *pkt = circ_bbuf_pop(&buf);
                            if(pkt != NULL){
                                //printf("Seq: %d\t Payload: %s\n", pkt->seq, pkt->payload);
                            }
                        }
                        printf("Listening on IP address: %s:%s\n\n", hbuf, sbuf);                  }
                }else{
                    rcv_msg ack_pkt = {
                        .flag = PKT_ACK,
                        .seq = expected_seq,
                    };
    
                    // Put together buffer view
                    for(int i = 0; i < buf.maxlen; i++){
                        ack_pkt.buffer[i] = (circ_bbuf_get(&buf, i) != NULL);
                    }
                    //session_len = sizeof(session_addr);
    
                    sendto_dbg(sock, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&session_addr,
                           session_len);
                    //printf("Sending Cumulative Acknowledgement Packet\n");
                }
            }/* else{
                //printf("timeout...nothing received for 10 seconds.\n");
                gettimeofday(&now, NULL);
                if (Cmp_time(last_recv_time, Zero_time) > 0) {
                    timersub(&now, &last_recv_time, &diff_time);
                    //printf("last msg received %lf seconds ago.\n\n",
                            //diff_time.tv_sec + (diff_time.tv_usec / 1000000.0));
                }
            } */
        }
    }

    fclose(file);
    for(int i = 0; i < WINDOW_SIZE; i++){
        ncp_msg *pkt = circ_bbuf_pop(&buf);
        if(pkt != NULL){
            printf("Seq: %d\t Payload: %s\n", pkt->seq, pkt->payload);
        }
    }
    printf("Exiting");
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {
    if (argc != 4) {
        Print_help();
    }

    if (sscanf(argv[1], "%d", &Loss_rate) != 1) {
        Print_help();
    }

    Port_Str = argv[2];

    if (!strncmp(argv[3], "WAN", 4)) {
        Mode = MODE_WAN;
    } else if (!strncmp(argv[3], "LAN", 4)) {
        Mode = MODE_LAN;
    } else {
        Print_help();
    }
}

static void Print_help(void) {
    printf("Usage: rcv <loss_rate_percent> <port> <env>\n");
    exit(0);
}

static int Cmp_time(struct timeval t1, struct timeval t2) {
    if      (t1.tv_sec  > t2.tv_sec) return 1;
    else if (t1.tv_sec  < t2.tv_sec) return -1;
    else if (t1.tv_usec > t2.tv_usec) return 1;
    else if (t1.tv_usec < t2.tv_usec) return -1;
    else return 0;
}


/* Circular Buffer Functions modified from https://embedjournal.com/implementing-circular-buffer-embedded-c/ */
int circ_bbuf_push(circular_buffer *cb, ncp_msg *packet, int i){
    int index = (cb->head + i) % cb->maxlen;

    // Check for buffer full (next would overwrite head)
    if (cb->buffer[index] != NULL) {
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


/* Use to shift buffer and deliver to application */
ncp_msg *circ_bbuf_pop(circular_buffer *cb){
    if (cb->buffer[cb->head] == NULL) {
        return NULL;  // buffer empty
    }

    ncp_msg *pkt = cb->buffer[cb->head];
    cb->buffer[cb->head] = NULL;  // mark as empty
    cb->head = (cb->head + 1) % cb->maxlen;
    return pkt;
}// Modified to only pop head of circular buffer, and moving head next index.

/* Used for NACK creation */
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

/* buffer[Head] should always be NULL when entering this method, will be called when expected packet comes in 
   delivers packets that can be delivered to the application */
void circ_bbuf_shift(circular_buffer *cb, FILE* file){
    ncp_msg *pkt;
    cb->head = (cb->head + 1) % cb->maxlen;
    pkt = cb->buffer[cb->head];
    while(pkt != NULL){
        // Write to file
        fwrite(pkt->payload, 1, strlen(pkt->payload), file);
        written_bytes += strlen(pkt->payload);

        // Set to null and shift
        cb->buffer[cb->head] = NULL;
        cb->head = (cb->head + 1) % cb->maxlen;
        expected_seq++;

        // Move to next slot.
        pkt = cb->buffer[cb->head];
    }
}



