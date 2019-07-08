/*!
  \file skeleton.c

  \brief Framework
*/
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <linux/inotify.h>
#include <string.h>
#include <syslog.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <termios.h>
#include <assert.h>
#include <time.h>
#include <math.h>
#include <signal.h>

#include <nanomsg/nn.h>
#include <nanomsg/pipeline.h>
#include <nanomsg/reqrep.h>

/* DEFINES ---------------------------------------------------------------- */

// #define DEBUG 1

#include "nanodefs.h"


/* GLOBALS ---------------------------------------------------------------- */


static volatile int loop = 1; /*!< Variable loop is set to 0 on SIG... */


/* CODE ------------------------------------------------------------------- */


/*!
  On SIGINT, loop is set to 0, and the threads and program quit.
*/
void interupt_handler(int dummy) {
  loop = 0;
}

unsigned long time_s() {
  return (unsigned long)time(NULL); // epoch secs
}


static void LOG( const char* format, ... ) {
  va_list args;
  char buf[8192];
 
  va_start(args,format);
 
  vsprintf(buf,format,args);
  openlog("cloudlog", LOG_PID | LOG_CONS, LOG_USER);
  syslog(LOG_INFO, "%s", buf);
  closelog();
#ifdef DEBUG
  printf("%s\n", buf);
  fflush(stdout);
#endif
  va_end(args);
}
 
 
/*!
  main
 */
int main( int argc, char *argv[] ) {
  int  c;
  int  len;
  int  res;
  struct timeval timeout;
  fd_set read_fds, write_fds, except_fds;
  int msg_num = 0;
  unsigned int startup_sleep = 24; // seconds

  // Interrupt handler
  signal(SIGINT,  interupt_handler);
  signal(SIGQUIT, interupt_handler);
  signal(SIGKILL, interupt_handler);

  while ( (c = getopt(argc, argv, "d:s:")) != -1) {
    switch (c) {
    case 's':
      startup_sleep = atoi(optarg);
      break;
    default:
      LOG("?? getopt returned character code 0%o ??\n", c);
    }
  }

  // Zzz
  LOG( "Starting in %i", startup_sleep );
  sleep( startup_sleep );

  // Maybe ended while waiting
  if ( ! loop ) {
    LOG( "Interrupted, exit" );
    return 1;
  }

  /*
    Initial server-client communication is through control socket.
  */
  int control_sock = nn_socket( AF_SP, NN_REQ );
  if ( control_sock < 0 ) {
    printf( "Cannot open control socket, errno %i (%s)\n", errno, strerror(errno) );
    return 1;
  }
  res = nn_connect( control_sock, "ipc:///tmp/control.ipc" );
  if ( res < 0 ) {
    printf( "Cannot connect to control socket, errno %i (%s)\n", errno, strerror(errno) );
    return 1;
  }

  // Request a comms channel
  char msg[MSG_SIZE];
  memset( msg, 0, sizeof(msg) );
  snprintf( msg, MSG_SIZE, "REQCHANNEL" );
  int bytes = nn_send( control_sock, msg, strlen(msg), 0 );
  if ( bytes < 0 ) {
    printf( "Cannot send REQCHANNEL, errno %i (%s)", errno, strerror(errno) );
    return 1;
  }

  // Receive answer from hub
  memset( msg, 0, sizeof(msg) );
  bytes = nn_recv( control_sock, &msg, MSG_SIZE, 0 );
  if ( bytes < 0 ) {
    printf( "Cannot receive REQCHANNEL, errno %i (%s)\n", errno, strerror(errno) );
    return 1;
  }
  printf( "RECEIVED %s\n", msg );
  nn_shutdown(control_sock, 0);

  if ( strcmp(msg, "NOCHANNEL") == 0 ) {
    printf( "No channels available\n" );
    return 1;
  }

  // The received msg contains the endpoint, connect to it.
  int sock = nn_socket(AF_SP, NN_PUSH);
  if ( sock < 0 ) {
    printf( "Cannot open nn_socket, errno %i (%s)\n", errno, nn_strerror(errno) );
    return 1;
  }
  res = nn_connect(sock, msg);
  if ( res < 0 ) {
    printf( "Cannot nn_connect, errno %i (%s)\n", errno, nn_strerror(errno) );
    return 1;
  }
  char *phy = strdup(msg);
  // At this point, we should have access to nano socket.
 
  while ( loop ) {
 
    memset( msg, 0, sizeof(msg) );
    long id = time_s();
    int fl = abs(rand()) % 64;
    //snprintf(msg, MSG_SIZE, "fuel_level=%i;\n", fl); // must be direct
    snprintf( msg, MSG_SIZE, "{\"fuel_level\":%i,\"command\":\"set\",\"id\":%lu}", fl, id );
    bytes = nn_send(sock, msg, strlen(msg), 0);
    ++msg_num;

    sleep(2);
    
  } // while(loop)

  memset( msg, 0, sizeof(msg) );
  snprintf( msg, MSG_SIZE, "ENDCHANNEL %s", phy );
  bytes = nn_send( control_sock, msg, strlen(msg), 0 );
  if ( bytes < 0 ) {
    printf( "Cannot send ENDCHANNEL, errno %i (%s)", errno, strerror(errno) );
    return 1;
  }
  free( phy );

  LOG( "Exit program." );
  return 0;
}
 
/* ---- EOT ---------------------------------------------------------------- */
