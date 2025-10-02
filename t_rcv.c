#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_include.h"

#define BACKLOG 1

static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static char *Port_Str;

int main(int argc, char *argv[])
{
    struct addrinfo    hints, *servinfo, *servaddr;
    int                listen_sock;
    int                recv_sock;
    int                ret;
    int                written_bytes, milestone, prev_milestone_bytes;
    char               mess_buf[MAX_MESS_LEN];
    long               on=1;
    struct sockaddr_storage from_addr;
    socklen_t               from_len;
    struct timeval          now;
    struct timeval          step;
    struct timeval          start;
    char                    hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    FILE*               file;
    char*                Dst_filename;

    /* Parse commandline args */
    Usage(argc, argv);
    printf("Successfully initialized with:\n");
    printf("\tPort = %s\n", Port_Str);

    /* Set up hints to use with getaddrinfo */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* we'll use AF_INET for IPv4, but can use AF_INET6 for IPv6 or AF_UNSPEC if either is ok */
    hints.ai_socktype = SOCK_STREAM; /* SOCK_STREAM for TCP (vs SOCK_DGRAM for UDP) */
    hints.ai_flags = AI_PASSIVE; /* indicates that I want to get my own IP address */

    /* Use getaddrinfo to get list of my own IP addresses */
    ret = getaddrinfo(NULL, Port_Str, &hints, &servinfo);
    if (ret != 0) {
       fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(ret));
       exit(1);
    }

    /* Loop over list of available addresses and take the first one that works
     * */
    for (servaddr = servinfo; servaddr != NULL; servaddr = servaddr->ai_next) {
        /* print IP, just for demonstration */
        ret = getnameinfo(servaddr->ai_addr, servaddr->ai_addrlen, hbuf,
                sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST |
                NI_NUMERICSERV);
        if (ret != 0) {
            fprintf(stderr, "getnameinfo error: %s\n", gai_strerror(ret));
            exit(1);
        }
        printf("Got my IP address: %s:%s\n\n", hbuf, sbuf);

        /* setup socket based on addr info. manual setup would look like:
         *   socket(PF_INET, SOCK_STREAM, IPPROTO_TCP) */
        listen_sock = socket(servaddr->ai_family, servaddr->ai_socktype, servaddr->ai_protocol);
        if (listen_sock < 0) {
            perror("tcp_server: socket");
            continue;
        }

        /* Allow binding to same local address multiple times */
        if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on)) < 0)
        {
            perror("tcp_server: setsockopt error \n");
            close(listen_sock);
            continue;
        }

        /* bind to receive incoming messages on this socket */
        if (bind(listen_sock, servaddr->ai_addr, servaddr->ai_addrlen) < 0) {
            perror("tcp_server: bind");
            close(listen_sock);
            continue;
        }
        
        /* Start listening */
        if (listen(listen_sock, BACKLOG) < 0) {
            perror("tcp_server: listen");
            close(listen_sock);
            continue;
        }

        break; /* got a valid socket */
    }
    if (servaddr == NULL) {
        fprintf(stderr, "No valid address found...exiting\n");
        exit(1);
    }

    written_bytes = 0;
    milestone = 1;
    prev_milestone_bytes = 0;

    for(;;)
    {
        /* Accept a connection */
        from_len = sizeof(from_addr);
        recv_sock = accept(listen_sock, (struct sockaddr *)&from_addr, &from_len);

        /* Print for demonstration */
        ret = getnameinfo((struct sockaddr *)&from_addr, from_len, hbuf,
                sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST |
                NI_NUMERICSERV);
        if (ret != 0) {
            fprintf(stderr, "getnameinfo error: %s\n", gai_strerror(ret));
            exit(1);
        }
        printf("Accepted connection from from %s:%s\n\n", hbuf, sbuf);
        bool first = true;
        gettimeofday(&start, NULL);

        for(;;)
        {
            /* IMPORTANT NOTE: recv is *not* guaranteed to receive a complete
             * message in single call */
            ret = recv(recv_sock, mess_buf, sizeof(mess_buf)-1, 0);
            mess_buf[ret] = '\0'; // Fixes naming issue
            if (ret <= 0) {
                printf("Client closed connection...closing socket...\n");
                close(recv_sock);
                break;
            }
            if (first){
                printf("Destination file: %s\n", mess_buf);

                Dst_filename = malloc(sizeof(mess_buf) + 1);
                strcpy(Dst_filename, mess_buf);
                file = fopen(Dst_filename, "wb");
                if (!file) {
                    perror("fopen");
                    exit(EXIT_FAILURE);
                }
                first = false;
            } else {
                written_bytes += sizeof(mess_buf);
                fwrite(mess_buf, 1, sizeof(mess_buf)-1, file);

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
            }
            //printf("received %d bytes: \n", ret); 
            //printf("---------------- \n");
        }
    }

    gettimeofday(&now, NULL);
    printf("Total time to transmit %f seconds\n", now.tv_sec - start.tv_sec + ((float) now.tv_usec - start.tv_usec)/1000000);
    printf("File Bytes transmitted successfully: %d\n", written_bytes);
    float throughput = (written_bytes) * 8 / 1000000 / (now.tv_sec - start.tv_sec + ((float) now.tv_usec - start.tv_usec)/1000000);
    printf("Total throughput: %f Mb/sec\n\n", throughput);

    return 0;

}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {
    if (argc != 2) {
        Print_help();
    }

    Port_Str = argv[1];
}

static void Print_help(void) {
    printf("Usage: t_rcv <port>\n");
    exit(0);
}
