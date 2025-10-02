#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "net_include.h"

static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;
static char *Server_IP;

int main(int argc, char *argv[])
{
    /* Initialize */
    Usage(argc, argv);
    printf("Successfully initialized with:\n");
    printf("\tSource filename = %s\n", Src_filename);
    printf("\tDestination filename = %s\n", Dst_filename);
    printf("\tHostname = %s\n", Hostname);
    printf("\tPort = %s\n", Port_Str);

    int                sock;
    struct addrinfo    hints, *servinfo, *servaddr;
    int                bytes_sent, ret, prev_milestone_bytes, transmitted_bytes;
    int                mess_len;
    struct timeval     now;
    struct timeval     start;
    struct timeval     step; // Used for 10MB steps
    char               mess_buf[MAX_MESS_LEN];
    char               hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    int                milestone;

    /* Parse commandline args */
    Usage(argc, argv);
    printf("Sending to %s at port %s\n", Server_IP, Port_Str);

    /* Set up hints to use with getaddrinfo */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* we'll use AF_INET for IPv4, but can use AF_INET6 for IPv6 or AF_UNSPEC if either is ok */
    hints.ai_socktype = SOCK_STREAM; /* SOCK_STREAM for TCP (vs SOCK_DGRAM for UDP) */
    hints.ai_protocol = IPPROTO_TCP;

    /* Use getaddrinfo to get IP info for string IP address (or hostname) in
     * correct format */
    ret = getaddrinfo(Server_IP, Port_Str, &hints, &servinfo);
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
        printf("Found server address: %s:%s\n\n", hbuf, sbuf);

        /* setup socket based on addr info. manual setup would look like:
         *   socket(PF_INET, SOCK_STREAM, IPPROTO_TCP) */
        sock = socket(servaddr->ai_family, servaddr->ai_socktype, servaddr->ai_protocol);
        if (sock < 0) {
            perror("tcp_client: socket");
            continue;
        }

        /* Connect to server */
        ret = connect(sock, servaddr->ai_addr, servaddr->ai_addrlen);
        if (ret < 0)
        {
            perror("tcp_client: could not connect to server"); 
            close(sock);
            continue;
        }
        printf("Connected!\n\n");

        gettimeofday(&step, NULL);
        gettimeofday(&start, NULL);
        
        break; /* got a valid socket */
    }

    if (servaddr == NULL) {
        fprintf(stderr, "No valid address found...exiting\n");
        exit(1);
    }

    //printf("%s\n", Dst_filename);
    printf("Sending filename: '%s' (%zu bytes)\n", Dst_filename, strlen(Dst_filename));
    ret = send(sock, Dst_filename, strlen(Dst_filename), 0);
    if (ret < 0) {
        perror("tcp_client: error in sending\n");
        exit(1);
    }
    printf("Send filename\n");

    FILE* file = fopen(Src_filename, "rb");
    struct stat file_stats; 
    stat(Src_filename, &file_stats);

    transmitted_bytes = 0;
    milestone = 1;
    prev_milestone_bytes = 0;

    while(!feof(file))
    {
        /* Read message into mess_buf */
        //printf("enter message: ");
        size_t bytes_read = fread(mess_buf, sizeof(char), MAX_MESS_LEN-1, file);
        //printf("%d",bytes_read);
        transmitted_bytes += bytes_read;
        if (transmitted_bytes > milestone * MB * 10) {
            gettimeofday(&now, NULL);
            printf("File Bytes transmitted successfully: %d\n", transmitted_bytes);
            float throughput = (transmitted_bytes-prev_milestone_bytes) * 8 / 1000000 / (now.tv_sec - step.tv_sec + ((float) now.tv_usec - step.tv_usec)/1000000);
            printf("Throughput of last 10MB: %f Mb/sec\n\n", throughput);//

            // Reset or advance variables
            milestone++;
            prev_milestone_bytes = transmitted_bytes;
            gettimeofday(&step, NULL);        }
        /* Send message */
        printf("Bytes Sent: %d\n", bytes_sent);
        ret = send(sock, &mess_buf[bytes_sent], bytes_read, 0);
        if (ret < 0) {
            perror("tcp_client: error in sending\n");
            exit(1);
        }
        
    }

    gettimeofday(&now, NULL);
    printf("Total time to transmit %f seconds\n", now.tv_sec - start.tv_sec + ((float) now.tv_usec - start.tv_usec)/1000000);
    printf("Total Transmitted Bytes w/ retransmissions: %d\n", transmitted_bytes);
    float throughput = (transmitted_bytes) * 8 / 1000000 / (now.tv_sec - start.tv_sec + ((float) now.tv_usec - start.tv_usec)/1000000);
    printf("Total throughput: %f Mb/sec\n\n", throughput);

    return 0;

}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {
    if (argc != 3) {
        Print_help();
    }

    Src_filename = argv[1];

    // Make a modifiable copy of the destination argument
    char *dest_str = strdup(argv[2]);  // Keep original modifiable copy
    if (!dest_str) {
        perror("strdup failed");
        exit(EXIT_FAILURE);
    }

    Dst_filename = strtok(dest_str, "@");
    Hostname     = strtok(NULL, ":");
    Port_Str     = strtok(NULL, ":");

    if (!Dst_filename || !Hostname || !Port_Str) {
        fprintf(stderr, "Error: Malformed destination string. Expected format: <file>@<ip>:<port>\n");
        Print_help();
    }

    // Save Server_IP separately
    Server_IP = Hostname;

}

static void Print_help(void) {
    printf("Usage: t_ncp <source_file_name> <dest_file_name>@<ip_addr>:<port>\n");
    exit(0);
}
