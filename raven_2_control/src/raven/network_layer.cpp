/**********************************************
 *
 * file: network_layer.cpp
 * author: Hawkeye King
 * create date: 3/13/2004
 *
 *  I read data out of fifo0 and put it on the network
 *  I listen to a network socket and put incoming data on fifo1
 *
 *  I output a message every 1000 packets
 *
 *********************************************

start
  open socket on port 36000
  listen on socket.
  print whatever comes in over socket.
end

 *********************************************/

#include <sys/types.h>  // provides FD_SET, FD_CLR, etc.
#include <sys/socket.h> // provides socket constants
#include <sys/time.h>   // provides timers
#include <netinet/in.h> // defines socket ip protocols/address structs
#include <netdb.h>      // port/hostname lookup features.
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <ros/ros.h>     // Use ROS
#include <ros/console.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
//#include <rtai_fifos.h>

#include "itp_teleoperation.h"
#include "DS0.h"
#include "DS1.h"
#include "log.h"

#define SERVER_PORT  "36000"
//#define SERVER_ADDR  "192.168.0.102"
#define SERVER_ADDR  "128.95.205.206"

extern int recieveUserspace(void *u,int size);

int initSock (const char* port )
{
    int request_sock;
    struct servent *servp;       // stores port & protocol
    struct sockaddr_in server;   // socket address in AF_<family>

    //------------ init --------------//

    // create a socket descriptor, uninitialized.
    if ((request_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
    {
        perror("socket");
        return 0;
    }

    // initialize port/protocol struct.
    if (isdigit(port[0]))
    {
        static struct servent s;
        servp = &s;
        s.s_port = htons((u_short)atoi(port));
    }
    else if ((servp = getservbyname(port,"tcp")) == 0)   // service lookup
    {
        perror("socket");
        return 0;
    }

    memset((void*) &server, 0, sizeof server);// initialize to null/all zeros?
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = servp->s_port;

    // "bind" the port to the socket.
    if (bind(request_sock, (struct sockaddr *) &server, sizeof server) < 0)
    {
        perror("bind");
        return 0;
    }

    //----------- end init -------------//



    return request_sock;
}

// Calculate checksum for a teleop packet
int UDPChecksum(struct u_struct *u)
{
    int chk=0;
    chk =  (u->surgeon_mode);
    chk += (u->delx[0])+(u->dely[0])+(u->delz[0]);
    chk += (u->delx[1])+(u->dely[1])+(u->delz[1]);
    chk += (u->buttonstate[0]);
    chk += (u->buttonstate[1]);
    chk += (int)(u->sequence);
    return chk;
}


// Chek packet validity, incl. sequence numbering and checksumming
//int checkPacket(struct u_struct &u, int seq);

// main //
volatile struct v_struct v;

void* network_process(void* param1)
{
    int sock;              // sockets.
    int nfound, maxfd;
    const char *port = SERVER_PORT;
    fd_set rmask, mask;
    static struct timeval timeout = { 0, 500000 }; // .5 sec //
    struct u_struct u;

    int uSize=sizeof(struct u_struct);

    struct sockaddr_in clientName;
    int clientLength=sizeof(clientName);
    int retval;
    static int k = 0;
    int logFile;
    struct timeval tv;
    struct timezone tz;
    char logbuffer[100];
    unsigned int seq = 0;
    volatile int bytesread;

    // print some status messages
    ROS_INFO("Starting network services...");
    ROS_INFO("  u_struct size: %i",uSize);
    ROS_INFO("  Using default port %s",port);

    ///// open log file
    logFile = open("err_network.log", O_WRONLY | O_CREAT | O_APPEND  | O_NONBLOCK , 0664);
    if ( logFile < 0 )
    {
        ROS_ERROR("ERROR: could not open log file.\n");
        exit(1);
    }
    gettimeofday(&tv,&tz);
    sprintf(logbuffer, "\n\nOpened log file at %s\n", ctime(&(tv.tv_sec)) );
    retval = write(logFile,logbuffer, strlen(logbuffer));

    /////  open socket
    sock = initSock(port);
    if ( sock <= 0)
    {
        ROS_ERROR("socket: service failed to initialize socket. (%d)\n",logFile);
        exit(1);
    }

    ///// setup sendto address
    memset(&clientName, 0, sizeof(clientName));
    clientName.sin_family = AF_INET;
    inet_aton(SERVER_ADDR, &clientName.sin_addr);
    clientName.sin_port = htons((u_short)atoi(port));

    clientLength=sizeof(clientName);

    ///// initialize data polling
    FD_ZERO(&mask);
    FD_SET(sock, &mask);
    maxfd=sock;

    ROS_INFO("Network layer ready.");

    ///// Main read/write loop
    while ( ros::ok() )
    {
        rmask = mask;
        timeout.tv_sec = 2;  // hack:reset timer after timeout event.
        timeout.tv_usec = 0; //        ""

        // wait for i/o lines to change state //
        nfound = select(maxfd+1, &rmask, (fd_set *)0, (fd_set *)0, &timeout);

        // Select error
        if (nfound < 0)
        {
            if (errno == EINTR)
            {
                printf("interrupted system call\n");
                continue;
            }
            perror("select");
            break;
        }

        // Select timeout: nothing to do
        if (nfound == 0)
        {
            fflush(stdout);
            continue;
        }

        // Select: data on socket
        if (FD_ISSET( sock, &rmask))
        {
            bytesread = recvfrom(sock,
                                 &u,
                                 uSize,
                                 0,
                                 NULL,
                                 NULL);
            if (bytesread != uSize){
                ROS_ERROR("ERROR: Rec'd wrong ustruct size on socket!\n");
                FD_CLR(sock, &rmask);
                continue;
            }

            k++;
            if (k==1) {
                log_msg("rec'd first socket data");
            } else if (k % 10000 == 0) {
                log_msg("rec'd socket data x10000");
            }

//
//            if (u.checksum != UDPChecksum(&u))   // Check checksum
//            {
//                gettimeofday(&tv,&tz);
//                sprintf(logbuffer, "%s Bad Checksum -> rejected packet\n", ctime(&(tv.tv_sec)) );
//                ROS_ERROR("%s Bad Checksum -> rejected packet\n", ctime(&(tv.tv_sec)) );
//                retval = write(logFile,logbuffer, strlen(logbuffer));
//
//            }
//            else
            if (u.sequence == 0)        // Zero seqnum means reflect packet to sender
            {
                gettimeofday(&tv,&tz);
                sprintf(logbuffer, "%s Zero sequence -> reflect packet\n", ctime(&(tv.tv_sec)) );
                ROS_INFO("%s Zero sequence -> reflect packet\n", ctime(&(tv.tv_sec)) );

                retval = write(logFile,logbuffer, strlen(logbuffer));

            }
            else if (u.sequence > seq+1)    // Skipping sequence number (dropped)
            {
                gettimeofday(&tv,&tz);
                sprintf(logbuffer, "%s Skipped (dropped?) packets %d - %d\n", ctime(&(tv.tv_sec)),seq+1, u.sequence-1 );
                ROS_WARN("%s Skipped (dropped?) packets %d - %d\n", ctime(&(tv.tv_sec)),seq+1, u.sequence-1 );
                retval = write(logFile,logbuffer, strlen(logbuffer));
                seq = u.sequence;

            }
            else if (u.sequence == seq)     // Repeated sequence number
            {
                gettimeofday(&tv,&tz);
                sprintf(logbuffer, "%s Duplicated packet %d - %d\n", ctime(&(tv.tv_sec)),seq, u.sequence );
                ROS_ERROR("%s Duplicated packet %d - %d\n", ctime(&(tv.tv_sec)),seq, u.sequence );
                retval = write(logFile,logbuffer, strlen(logbuffer));

            }

            else if (u.sequence > seq)       // Valid packet
            {
                seq = u.sequence;
                recieveUserspace(&u,uSize);
            }

            else if (seq > 1000 && u.sequence < seq-1000)       // reset sequence(skipped more than 1000 packets)
            {
                gettimeofday(&tv,&tz);
                sprintf(logbuffer, "%s Sequence numbering reset from %d to %d\n", ctime(&(tv.tv_sec)),seq, u.sequence );
                ROS_INFO("%s Sequence numbering reset from %d to %d\n", ctime(&(tv.tv_sec)),seq, u.sequence );
                seq = u.sequence;
                retval = write(logFile,logbuffer, strlen(logbuffer));
            }
            else
            {
                gettimeofday(&tv,&tz);
                sprintf(logbuffer, "%s Out of sequence packet %d\n", ctime(&(tv.tv_sec)),seq );
                ROS_ERROR("%s Out of sequence packet %d\n", ctime(&(tv.tv_sec)),seq );
                retval = write(logFile,logbuffer, strlen(logbuffer));
            }
        }

#ifdef NET_SEND
        sendto ( sock, (void*)&v, vSize, 0,
                 (struct sockaddr *) &clientName, clientLength);
#endif

    } // while(1)

    close(sock);

    ROS_INFO("Network socket is shutdown.");
    return(NULL);
} // main - server.c //
























/* int checkPacket(struct u_struct &u, int seq) */
/* { */
/*   struct timeval tv; */
/*   struct timezone tz; */

/*   if (u.checksum != UDPChecksum(&u) ) { // Check checksum */
/*     gettimeofday(&tv,&tz); */
/*     sprintf(logbuffer, "%s Bad Checksum -> rejected packet\n", ctime(&(tv.tv_sec)) ); */
/*     printf("%s Bad Checksum -> rejected packet\n", ctime(&(tv.tv_sec)) ); */
/*     write(logFile,logbuffer, strlen(logbuffer)); */

/*   } else if (u.sequence == 0) {      // Zero seqnum means reflect packet to sender */
/*     gettimeofday(&tv,&tz); */
/*     sprintf(logbuffer, "%s Zero sequence -> reflect packet\n", ctime(&(tv.tv_sec)) ); */
/*     printf("%s Zero sequence -> reflect packet\n", ctime(&(tv.tv_sec)) ); */

/*     write(logFile,logbuffer, strlen(logbuffer)); */

/*   } else if (u.sequence > seq+1){   // Skipping sequence number (dropped) */
/*     gettimeofday(&tv,&tz); */
/*     sprintf(logbuffer, "%s Skipped (dropped?) packets %d - %d\n", ctime(&(tv.tv_sec)),seq+1, u.sequence-1 ); */
/*     printf("%s Skipped (dropped?) packets %d - %d\n", ctime(&(tv.tv_sec)),seq+1, u.sequence-1 ); */
/*     write(logFile,logbuffer, strlen(logbuffer)); */
/*     seq = u.sequence; */

/*   } else if (u.sequence == seq){    // Repeated sequence number */
/*     gettimeofday(&tv,&tz); */
/*     sprintf(logbuffer, "%s Duplicated packet %d - %d\n", ctime(&(tv.tv_sec)),seq ); */
/*     printf("%s Duplicated packet %d - %d\n", ctime(&(tv.tv_sec)),seq ); */
/*     write(logFile,logbuffer, strlen(logbuffer)); */

/*   } else if (u.sequence > seq){      // Valid packet */
/*     seq = u.sequence; */
/*     retval=write(fifo1, &u, uSize); // Write data to module via FIFO 1. */

/*     // Print some diagnostics. */
/*     // printf("%d\t%d\t%d\t\n", u.delx[0], u.dely[0], u.delz[0]); */
/*     if (retval != uSize) */
/*       printf("error writing fifo 1: expected %d rec'd %d\n", uSize, retval); */

/*   } else { */
/*     gettimeofday(&tv,&tz); */
/*     sprintf(logbuffer, "%s Out of sequence packet %d\n", ctime(&(tv.tv_sec)),seq ); */
/*     printf("%s Out of sequence packet %d\n", ctime(&(tv.tv_sec)),seq ); */
/*     write(logFile,logbuffer, strlen(logbuffer)); */

/*   } */

/* }  */
