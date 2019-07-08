/*!
  \file hub.c

  \brief Server which pushes messags from clients to cloud.

  Uses cloud-device for communications to cloud, which
  could/will/might be incorprated in this code in the future.

  Starts a control socket listening for requests
  for comms channel.

  On request, checks thread avaiability in pool.

  If available, returns a channel and starths the thread listening on
  the channel. Returns "<channel>". Channels <channel> are called
  "ipc:///tmp/nanotest<T>.ipc" where <T> = [0, POOL_SIZE).

  If no thread availble, returns "NOCHANNEL".

  Connections can be closed by sending "ENDCHANNEL ipc:///tmp/nanotest<T>.ipc" to
  the control socket.

  Threads which timeout or which have been closed are made available again.

  Test with <tt><b>nanocat</b></tt>:
  \code
    ->  nanocat --req --connect "ipc:///tmp/control.ipc" --data "REQCHANNEL" --format ascii
    <-  ipc:///tmp/nanotest0.ipc
    ->  nanocat --push --connect "ipc:///tmp/nanotest0.ipc" --data "HI"
  \endcode

  Or with fly</tt>:
  \code
    -> ./hub
  Start listening
    -> ./fly
    <- from fly window:
    RECEIVED ipc:///tmp/nanotest0.ipc
    <- from hub window:
  RECEIVED REQCHANNEL
  REQCHANNEL received
  n=1
  ipc:///tmp/nanotest0.ipc
  listener(ipc:///tmp/nanotest0.ipc)
  listener, starting loop
  RECEIVED ipc:///tmp/nanotest0.ipc 0 (Hi 9)
  RECEIVED ipc:///tmp/nanotest0.ipc 1 (Hi 8)
  RECEIVED ipc:///tmp/nanotest0.ipc 2 (Hi 7)
  RECEIVED ipc:///tmp/nanotest0.ipc 3 (Hi 6)
  RECEIVED ipc:///tmp/nanotest0.ipc 4 (Hi 5)
  RECEIVED ipc:///tmp/nanotest0.ipc 5 (Hi 4)
  RECEIVED ipc:///tmp/nanotest0.ipc 6 (Hi 3)
  RECEIVED ipc:///tmp/nanotest0.ipc 7 (Hi 2)
  RECEIVED ipc:///tmp/nanotest0.ipc 8 (Hi 1)
  RECEIVED ipc:///tmp/nanotest0.ipc 9 (Hi 0)
  TIMEOUT ipc:///tmp/nanotest0.ipc
  ipc:///tmp/nanotest0.ipc EXIT
  \endcode 
*/
#include <assert.h>
#include <syslog.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <nanomsg/nn.h>
#include <nanomsg/pipeline.h>
#include <nanomsg/reqrep.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include <poll.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <nanomsg/bus.h>


/* DEFINES ---------------------------------------------------------------- */


#include "nanodefs.h"

//! Administrative data for thread.
struct t_data {
  int          status;    /*!< Status value T_<status> */
  long         ts_last;   /*!< Timestamp of last message received (seconds) */
  char        *phy;       /*!< String containing the endpoint */
  pthread_t    thread;    /*!< Pointer to thread */
  int          sock;      /*!< Nanomsg socket */
  int          endpoint;  /*!< Nanomsg endpoint */
} t_data;


/* GLOBALS ---------------------------------------------------------------- */


static volatile int go = 1; /*!< Variable go is set to 0 on SIGINT */


/* CODE ------------------------------------------------------------------ */


static void LOG( const char* format, ... ) {
  va_list args;
  char buf[8192];
 
  va_start(args,format);
 
  vsprintf(buf,format,args);
  openlog("hub", LOG_PID | LOG_CONS, LOG_USER);
  syslog(LOG_INFO, "%s", buf);
  closelog();
#ifdef DEBUG
  printf("%s\n", buf);
  fflush(stdout);
#endif
  va_end(args);
}

/*!
  On SIGINT, go is set to 0, and the threads and program quit.
*/
void interupt_handler(int dummy) {
  go = 0;
}

/*!
  Returns a long with the current time stamp in
  seconds.
*/
long int ts_secs() {
  struct timespec tp;
  clockid_t clk_id;
  clk_id = CLOCK_MONOTONIC;

  //clock_getres(clk_id, &tp);
  //printf( "Resolution is %ld nano seconds.\n", tp.tv_nsec  );

  clock_gettime(clk_id, &tp);
  //printf("tp.tv_sec: %lld\n", (long long int)tp.tv_sec);
  //printf("tp.tv_nsec: %ld\n", (long int)tp.tv_nsec);

  return (long int)tp.tv_sec;
}

/*!
  Returns a long long int with the current time stamp in
  nano-seconds.
*/
long long int ts() {
  struct timespec tp;
  clockid_t clk_id;
  clk_id = CLOCK_MONOTONIC;

  clock_gettime(clk_id, &tp);
  //printf("tp.tv_sec: %lld\n", (long long int)tp.tv_sec);
  //printf("tp.tv_nsec: %ld\n", (long int)tp.tv_nsec);
  return ((long long int)tp.tv_sec*1000000000L)+(long int)tp.tv_nsec;
}

// Receive a nanomsg.
// Waits TIMEOUT seconds for answers.
int nnreceive( int sock, char *msg ) {
  struct pollfd  poll_fds[1];  
  int    nnsocket_recvfd;
  size_t sz = sizeof(int);

  nn_getsockopt( sock, NN_SOL_SOCKET, NN_RCVFD, &nnsocket_recvfd, &sz );
  poll_fds[0].fd     = nnsocket_recvfd;
  poll_fds[0].events = POLLIN;

  int poll_state = poll( poll_fds, 1, TIMEOUT );
  if ( poll_state > 0 ) {
    if ( poll_fds[0].revents & POLLIN ) {
      int bytes = nn_recv( sock, msg, MSG_SIZE, NN_DONTWAIT );
      if ( bytes > 0 ) {
	return bytes;
      }
    }
  }
  return poll_state;
}

/*!  
  Watches for timeouts in threads with status #T_WORKING. Sets
  thread's flag to #T_TIMEOUT on timeout, causing relevant thread to
  exit.

  \param Pointer to array with all t_data pointers.

  \note Disabled at the moment.
*/
void *watcher(void *thread_info) {
  int i;
  long ts_now;

  struct t_data **thread_data = (struct t_data**)thread_info;
  while( go ) {
    ts_now = ts_secs();
    for( i = 0; i < POOL_SIZE; i++ ) {
      if ( thread_data[i]->status == T_WORKING ) {
	/*printf( "WATCHER %s %i %li\n", thread_data[i]->phy, thread_data[i]->status, 
	  ts_now-thread_data[i]->ts_last );*/
	if ( ts_now-thread_data[i]->ts_last > 2 ) {
	  //thread_data[i]->status = T_TIMEOUT;
	}
	int res = pthread_kill(thread_data[i]->thread, 0);
	if ( res != 0 ) {
	  printf( "Thread %i gone\n", i );
	} else {
	  printf( "Thread kill returned %i\n", res );
	}
      }
    }
    sleep(2);
  }
  printf( "WATCHER EXIT\n" );
}

/*!
  Messages which appear here (sent by the listener threads) 
  are forwarded to the cloud.

  See them with:
    nanocat --ascii --bus --bind "ipc:///tmp/cloud.ipc"

  PJB TODO: better error handling
*/
void *bucketeer(void *bucket_sock) {
  char buf[MSG_SIZE];
  char msg[MSG_SIZE];

  // cloud connector, the cloud-device software takes care of
  // the connexion between cloud and SC7.
  int cloud_socket = nn_socket(AF_SP, NN_BUS);
  if (cloud_socket < 0) {
    LOG( "could not create cloud socket" );
    return 0;
  }
  assert(nn_connect(cloud_socket, "ipc:///tmp/cloud.ipc"));
  // end cloud connector

  LOG( "Bucketeer starting" );

  int sock = *((int*)bucket_sock);
  while( go ) {
    memset( buf, 0, sizeof(buf) );
    int bytes = nnreceive( sock, buf );

    if ( bytes > 0 ) {
#ifdef DEBUG
      printf( "HUB -> CLOUD (%s)\n", buf );
#endif
      if ( strlen(buf) > 1 ) { // do not send empty strings
	sprintf(msg, "%s\n", buf);
	nn_send( cloud_socket, msg, strlen(msg), 0 );
      }
    } // bytes > 0

  } // while go
  LOG( "Bucketeer exit" );
  return 0;
}

/*!
  Opens a socket, and connects to the endpoint.
  Then waits 5 seconds for input. If nothing received, times
  out and exits.

  When running, forwards received messages to the cloud. Times out after
  TIMEOUT seconds.

  \param Pointer to the t_data structure.

  \todo Variable timeout time.
 */
void *listener(void *the_data) {
  // handle data, send to the cloud, ...

  char buf[MSG_SIZE];
  struct t_data *my_data = (struct t_data*)the_data;
  int  c = 0;

  LOG( "New listener(%s)", my_data->phy );

  int sock = nn_socket( AF_SP, NN_PULL );
  if ( sock < 0 ) {
    LOG( "Cannot open socket, errno %i (%s)", errno, strerror(errno) );
    my_data->status = T_FAILED;
    return 0;
  }

  // Save in shared data
  my_data->sock = sock;

  int endpoint = nn_bind(sock, my_data->phy);
  if ( endpoint < 0 ) {
    LOG( "Cannot bind %s, errno %i (%s)", my_data->phy, errno, strerror(errno) );
    my_data->status = T_FAILED;
    return 0;
  }

  // Save in shared data
  my_data->endpoint = endpoint;


  // Bucket to write to, messages written here are forwarded to the cloud.
  int bucket_sock = nn_socket( AF_SP, NN_PUSH );
  if ( bucket_sock < 0 ) {
    LOG( "Cannot open bucket socket, errno %i (%s)", errno, strerror(errno) );
    my_data->status = T_FAILED;
    return 0;
  }
  int res = nn_connect( bucket_sock, "ipc:///tmp/bucket.ipc" );
  if ( res < 0 ) {
    LOG( "Cannot connect bucket socket, errno %i (%s)", errno, strerror(errno) );
    return 0;
  }
  // end bucket


  LOG( "listener, starting loop" );
  my_data->status = T_WORKING;
  while ( my_data->status == T_WORKING ) {
    memset( buf, 0, sizeof(buf) );
    int bytes = nnreceive( sock, buf );
    if ( bytes > 0 ) {
#ifdef DEBUG
      printf( "RECEIVED %s %i (%s)\n", my_data->phy, c, buf );
#endif
      c += 1;
      my_data->ts_last = ts_secs();
      /*
	Copy msg to bucket
      */
      nn_send( bucket_sock, buf, strlen(buf), 0 );
    } else if ( bytes <= 0 ) {
      /*
	No timeout for GPS data? That means this thread will
	never be available again from main().
      */
      LOG( "TIMEOUT %s", my_data->phy );
      my_data->status = T_TIMEOUT;
      break;
    }
  } // while T_WORKING

  res = nn_shutdown( sock, endpoint );
  if ( res < 0 ) {
    LOG( "Cannot shutdown %s, errno %i (%s)", my_data->phy, errno, strerror(errno) );
  }
  
  // shuffle off this mortal coil
  LOG( "EXIT %s",  my_data->phy );
  return 0;
}

/*!
  Main function.

  Opens control socket, listens for request, spawns threads.
*/
int main( int argc, char *argv[] ) {

  // PJB TODO fix/rethink interrupt handler
  //signal(SIGINT, interupt_handler);

  /*
    Control socket, gets request for comms.
    
    nanocat test:
    ->  nanocat --req --connect "ipc:///tmp/control.ipc" --data "REQCHANNEL" --format ascii
    <-  CHANNEL ipc:///tmp/nanotest0.ipc

    -> nanocat --push --connect "ipc:///tmp/nanotest0.ipc" --data "HI"
  */
  int control_sock = nn_socket( AF_SP, NN_REP );
  if ( control_sock < 0 ) {
    LOG( "Cannot open control socket, errno %i (%s)", errno, strerror(errno) );
    return 1;
  }
  int res = nn_bind( control_sock, "ipc:///tmp/control.ipc" );
  if ( res < 0 ) {
    LOG( "Cannot bind control socket, errno %i (%s)", errno, strerror(errno) );
    return 1;
  }

  /*
    Bucket for threads to dump messages to be send to cloud/...
    
    This one is NN_PULL, use NN_PUSH on the thread side.
  */
  int bucket_sock = nn_socket( AF_SP, NN_PULL );
  if ( bucket_sock < 0 ) {
    LOG( "Cannot open bucket socket, errno %i (%s)", errno, strerror(errno) );
    return 1;
  }
  res = nn_bind( bucket_sock, "ipc:///tmp/bucket.ipc" );
  if ( res < 0 ) {
    LOG( "Cannot bind bucket socket, errno %i (%s)", errno, strerror(errno) );
    return 1;
  }
  // end bucket socket

  struct t_data *thread_data[POOL_SIZE];
  pthread_t thread[POOL_SIZE];
  int i;
  for( i = 0; i < POOL_SIZE; i++) {
    thread[i] = (pthread_t)0;
    thread_data[i] = malloc(sizeof(struct t_data));
    thread_data[i]->status = T_AVAILABLE;
  }

  // Set up watcher thread here, disabled.
  //pthread_t watch_thread;
  //pthread_create( &watch_thread, NULL, watcher, (void*)thread_data );  

  // Set up bucket thread here
  pthread_t bucket_thread;
  pthread_create( &bucket_thread, NULL, bucketeer, (void*)&bucket_sock );  
  
  LOG( "Control start listening" );
  char msg[MSG_SIZE];

  while ( go ) {
    memset( msg, 0, sizeof(msg) );
    int bytes = nn_recv( control_sock, &msg, MSG_SIZE, 0 );
    if ( bytes < 0 ) {
      LOG( "Cannot nn_recv, errno %i (%s)", errno, strerror(errno) );
      go = 0;
      break;
    }

#ifdef DEBUG
    printf( "RECEIVED %s\n", msg );
#endif

    if ( strcmp("REQCHANNEL", msg) == 0 ) {
      LOG( "REQCHANNEL received" );

      // Check for empty channel
      int empty_channel = -1;
      for( i = 0; i < POOL_SIZE; i++) {
	//printf( "n=%i\n", thread_data[i]->status );
	if ( thread_data[i]->status == T_TIMEOUT ) {
	  thread_data[i]->status = T_AVAILABLE;
	}
	if ( thread_data[i]->status == T_FAILED ) { // really?
	  thread_data[i]->status = T_AVAILABLE;
	}
	if ( thread_data[i]->status == T_AVAILABLE ) {
	  empty_channel = i;
	  break;
	}
      } // for POOL_SIZE

      if ( empty_channel == -1 ) {
	LOG( "NO CHANNELS AVAILABLE" );
	memset( msg, 0, sizeof(msg) );
	snprintf( msg, MSG_SIZE, "NOCHANNEL" );
	int bytes = nn_send(control_sock, msg, strlen(msg), 0 );
	continue; // continue while
      }

      // Bind to end point, setup thread.
      char tmp[MSG_SIZE];
      memset( tmp, 0, sizeof(tmp) );
      snprintf( tmp, MSG_SIZE, "ipc:///tmp/nanotest%i.ipc", empty_channel );
      printf( "%s\n", tmp );
      thread_data[empty_channel]->phy     = strdup( tmp );
      thread_data[empty_channel]->status  = T_INITIALISING;
      thread_data[empty_channel]->ts_last = ts_secs();

      // start thread
      pthread_create( &thread[empty_channel], NULL, listener, (void*)thread_data[empty_channel] );
      thread_data[empty_channel]->thread = thread[empty_channel];

      // reply to client
      memset( msg, 0, sizeof(msg) );
      snprintf( msg, MSG_SIZE, "%s", tmp );
      int bytes = nn_send(control_sock, msg, strlen(msg), 0 ); //NN_DONTWAIT); // check timeout
    } // REQ_CHANNEL

    // ENDCHANNEL ipc:///tmp/nanotest2.ipc
    if ( strncmp("ENDCHANNEL", msg, 10) == 0 ) {
      char *channel = strdup( msg+11 );
      LOG( "SHUTDOWN %s", channel );

      for( i = 0; i < POOL_SIZE; i++) {
	if ( thread_data[i]->status == T_WORKING ) {
	  if ( strcmp(thread_data[i]->phy, channel) == 0 ) {
	    LOG( "SHUTTING DOWN %i", i );
	    res = nn_shutdown( thread_data[i]->sock, thread_data[i]->endpoint );
	    thread_data[i]->status = T_AVAILABLE;
	    break;
	  }
	}
      } // for
      free(channel);
    } // ENDCHANNEL

    // ...
    
  } // while ( go )

  LOG( "Hub exit while loop, shutting down threads" );

  for( i = 0; i < POOL_SIZE; i++) {
    int res = nn_shutdown( thread_data[i]->sock, thread_data[i]->endpoint );
    pthread_join(thread[i], NULL);
  }
  //pthread_join(watch_thread, NULL);

  LOG( "Hub exit" );
  return 0;
}
