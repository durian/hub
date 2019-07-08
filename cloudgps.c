/*!
  \file readgps.c

  \brief Reads GPS NMEA messages.

  Reads GPS messages, dumps them on a nanomsg socket in JSON format.
  Only pushes position message.

  PJB: Forked from nanomsg-test/readgps.c
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

// An nmea message may contain a maximum of
// 20 items, each with a maximum length of 16
// A message can be max 20*16, plus 19 commas.
//
#define MAX_ITEMS  20
#define MAX_LENGTH 16
#define GPS_BUFFER ( (MAX_ITEMS * MAX_LENGTH) + MAX_ITEMS )
#define MAX_TEXT_LEN 512
#define MAX_TIMEOUT 10

#define CURRENT  0
#define PREVIOUS 1

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

/*!
  Keep a running tab on GPS position in gps_pos_t.
 */
struct gps_pos_t {
  float lat[2];          /*!< Current[0] and previous[1] lat position */
  float lon[2];          /*!< Current[0] and previous[1] lon position */
  float dir[2];          /*!< Current and previous direction */
  float vel[2];          /*!< Current and previous velocity */
  int   fix;             /*!< Not zero if GPS fix */
  unsigned long updated; /*!< Timestamp of latest update */
  // etc
};

/*!
  String representation of position, direction and velocity.
*/
void gps_pos_str( struct gps_pos_t *gps_pos, char *buf ) {
  snprintf( buf, MAX_TEXT_LEN, "%10.6f %11.6f %5.1f %6.2f", 
	    gps_pos->lat[CURRENT], gps_pos->lon[CURRENT],
	    gps_pos->dir[CURRENT], gps_pos->vel[CURRENT] );
}

/*!
  Return 1 if position is different from previous position.

  decimal
  places   degrees          distance
  -------  -------          --------
  0        1                111  km
  1        0.1              11.1 km
  2        0.01             1.11 km
  3        0.001            111  m
  4        0.0001           11.1 m
  5        0.00001          1.11 m
  6        0.000001         11.1 cm
*/
int gps_pos_changed( struct gps_pos_t *gps_pos ) {
  // Around north we get for example 359...1 which is only a 2 deg change.
  // So we calc te diff, substract 360, and take the smallest of these two.
  // The 359..1 change will give 358 and 2, we take 2.
  float delta_dira  = fabs( gps_pos->dir[CURRENT] - gps_pos->dir[PREVIOUS] ); 
  float delta_dirb  = fabs( delta_dira - 360.0 ); 
  float delta_dir   = delta_dira < delta_dirb ? delta_dira : delta_dirb; 
  if (
      ( fabs(gps_pos->lat[CURRENT] - gps_pos->lat[PREVIOUS]) < 0.00001 )
      &&
      ( fabs(gps_pos->lon[CURRENT] - gps_pos->lon[PREVIOUS]) < 0.00001 )
      &&
      ( fabs(gps_pos->vel[CURRENT] - gps_pos->vel[PREVIOUS]) < 0.1 )
      &&
      ( delta_dir < 2 )
      ) {
    return 0; // same position/heading/velocity
  }
#ifdef DEBUG
  printf( "0: %10.8f %11.8f\n", gps_pos->lat[CURRENT], gps_pos->lon[CURRENT] );
  printf( "1: %10.8f %11.8f\n", gps_pos->lat[PREVIOUS], gps_pos->lon[PREVIOUS] );
  printf( "0: %10.6f %10.6f\n", gps_pos->vel[CURRENT], gps_pos->dir[CURRENT] );
  printf( "1: %10.6f %10.6f\n", gps_pos->vel[PREVIOUS], gps_pos->dir[PREVIOUS] );
  // 0: 56.29028702 12.84673119
  // 1: 56.29028702 12.84672928
  //      .00001526   .00000191
#endif
  return 1;
}

static void LOG( const char* format, ... ) {
  va_list args;
  char buf[8192];
 
  va_start(args,format);
 
  vsprintf(buf,format,args);
  openlog("gps", LOG_PID | LOG_CONS, LOG_USER);
  syslog(LOG_INFO, "%s", buf);
  closelog();
#ifdef DEBUG
  printf("%s\n", buf);
  fflush(stdout);
#endif
  va_end(args);
}
 
// Split a comma-separated message into parts.
//
int split( char *s, char *strs[MAX_ITEMS] ) {
  const char delims[] = ",";
  char buf[MAX_LENGTH];
  
  int i = 0;
  do {
    size_t field_len = strcspn(s, delims);
    if ( field_len > MAX_LENGTH ) {
      field_len = MAX_LENGTH;
    }
    sprintf(buf, "%.*s", (int)field_len, s);
    strs[i] = malloc(strlen(buf) + 1);
    strcpy(strs[i], buf);
    i += 1;
    s += field_len;
  } while ( (*s++) && (i < MAX_ITEMS) );
  return i;
}
 
// $GPRMC,084435.0,A,5617.428119,N,01250.815218,E,0.0,,190517,0.0,E,A*2D
// nmea_to_dec(5617.428119);  -> 56.290470
// nmea_to_dec(01250.815218); -> 12.846920
//
float nmea_to_dec( float nmea_coord ) {
  int b;
  float c;
  
  b = nmea_coord / 100; // 51 degrees
  c = (nmea_coord / 100 - b) * 100 ; //(51.536605 - 51)* 100 = 53.6605
  c /= 60; // 53.6605 / 60 = 0.8943417
  c += b; // 0.8943417 + 51 = 51.8943417
  return c;
}
 
/*
  ALS3-E supports the following GPS related NMEA sequences:
  • GPGGA - Global Positioning System Fix Data, Time, Position and fix related 
    data for a GNSS receiver
  • GPRMC - Recommended minimum data for GPS
  • GPGSV - Detailed satellite data
  • GPGSA - Overall satellite data
  • GPVTG - Vector track and speed over the Ground
 
  The device prefix "GL" is for GLONASS related data. "GN" refers to GPS and 
  GLONASS together. ALS3-E supports the following GLONASS related NMEA 
  sentences:
  • GLGSV - Detailed satellite data
  • GNGSA - Overall satellite data
  • GNGNS - Positioning System
*/
int  parse_nmea( char* nmea, char* ans, struct gps_pos_t* gps_pos ) {
  char *items[MAX_ITEMS];
  char msg[6];
 
  // We remove the *xx checksum from the end.
  if ( nmea[strlen(nmea)-3] == '*' ) {
    nmea[strlen(nmea)-3] = '\0';
  }
  int num_items = split( nmea, items );

  // Take talker ID and message ID
  strncpy(msg, items[0]+1, 5);
  msg[5] = '\0';
 
#ifdef DEBUG
  LOG( "Message %s, containing %i items", msg, num_items );
  LOG( "%s", nmea );
#endif
#ifndef DEBUG
  //printf( "%s\n", nmea );
  //fflush( stdout );
#endif

  // A number of these should be supressed if no fix. 
  // (maybe keep position struct which is filled in by nmea messages)
  //
  if ( strcmp("GPGGA", msg) == 0 ) {
    // $GPGGA,hhmmss.ss,llll.ll,a,yyyyy.yy,a,x,xx,x.x,x.x,M,x.x,M,x.x,xxxx*hh
    //      0         1       2 3        4 5 6  7 
    //                                       | quality of fix
    //
    if ( strcmp( "0", items[6]) == 0 ) {
      // LOG( "GPGGA; invalid fix" );
      gps_pos->fix = 0;
    } else if ( strcmp( "1", items[6]) == 0 ) {
      // GPS fix
      // LOG( "GPGGA; valid fix" );
      // Eample:
      //   $GPGGA,084453.0,5617.428042,N,01250.815248,E,1,04,2.1,21.5,M,39.0,M,,*60
      float lat = nmea_to_dec( atof(items[2]) );
      float lon = nmea_to_dec( atof(items[4]) );
      snprintf( ans, MAX_TEXT_LEN, "%s lat=%f %s lon=%f %s", msg, lat, items[3], lon, items[5] );
      gps_pos->lat[PREVIOUS] = gps_pos->lat[CURRENT];
      gps_pos->lat[CURRENT]  = lat * ( items[3][0] == 'N' ? 1. : -1. );
      gps_pos->lon[PREVIOUS] = gps_pos->lon[CURRENT];
      gps_pos->lon[CURRENT]  = lon * ( items[5][0] == 'E' ? 1. : -1. );
      gps_pos->fix = 1;
      gps_pos->updated = time_s();
    } else if ( strcmp( "2", items[6]) == 0 ) {
      // Diff. GPS fix
      // Hasn't been observed.
      float lat = nmea_to_dec( atof(items[2]) );
      float lon = nmea_to_dec( atof(items[4]) );
      snprintf( ans, MAX_TEXT_LEN, "%s lat=%f %s lon=%f %s", msg, lat, items[3], lon, items[5] );
      gps_pos->lat[PREVIOUS] = gps_pos->lat[CURRENT];
      gps_pos->lat[CURRENT]  = lat * ( items[3][0] == 'N' ? 1. : -1. );
      gps_pos->lon[PREVIOUS] = gps_pos->lon[CURRENT];
      gps_pos->lon[CURRENT]  = lon * ( items[5][0] == 'E' ? 1. : -1. );
      gps_pos->fix = 1;
      gps_pos->updated = time_s();
    }
  } else if ( strcmp("GPRMC", msg) == 0 ) {
    // $GPRMC,,V,,,,,,,,,,N*53
    // $GPRMC,hhmmss.ss,A,llll.ll,a,yyyyy.yy,a,x.x,x.x,ddmmyy,x.x,a*hh
    //      0         1 2
    //                  | Data status (V=navigation receiver warning)
    //
    if ( strcmp( "V", items[2]) == 0 ) {
      // LOG( "GPRMC; invalid data" );
      gps_pos->fix = 0;
    } else {
      float lat = nmea_to_dec( atof(items[3]) );
      float lon = nmea_to_dec( atof(items[5]) );
      snprintf( ans, MAX_TEXT_LEN, "%s lat=%f %s lon=%f %s", msg, lat, items[4], lon, items[6] );
      gps_pos->lat[PREVIOUS] = gps_pos->lat[CURRENT];
      gps_pos->lat[CURRENT]  = lat * ( items[4][0] == 'N' ? 1. : -1. );
      gps_pos->lon[PREVIOUS] = gps_pos->lon[CURRENT];
      gps_pos->lon[CURRENT]  = lon * ( items[6][0] == 'E' ? 1. : -1. );
      gps_pos->fix = 1;
      gps_pos->updated = time_s();
    }
  } else if ( strcmp("GPGSV", msg) == 0 ) {
    // $GPGSV,x,x,x,x,x,x,x,...*hh
    //        | num of messages
    //          | this message num
    //            | total number of satellites
    // Example:
    //  $GPGSV,4,1,15,01,06,040,20,12,,,33,13,41,160,45,15,59,220,31*41
    //  $GPGSV,4,2,15,17,37,104,40,19,26,130,37,24,47,274,18,30,02,094,31*71
    //  $GPGSV,4,3,15,33,,,36,39,,,36,40,,,35,10,18,319,*41
    //  $GPGSV,4,4,15,18,23,291,,20,05,223,,28,32,056,*43
#ifdef DEBUG
    LOG( "Satellites in view: %s", items[3] );
    int idx;
    for ( idx = 4; idx < num_items; idx += 4 ) {
      LOG( "PRN %s, ELE %s, AZI %s, SNR %s", items[idx], items[idx+1], items[idx+2], items[idx+3] );
      snprintf( ans, MAX_TEXT_LEN, "%s PRN %s, ELE %s, AZI %s, SNR %s", msg, items[idx], items[idx+1], items[idx+2], items[idx+3] );
    }
#endif
    snprintf( ans, MAX_TEXT_LEN, "%s Satellites in view: %s", msg, items[3] );
  } else if ( strcmp("GPGSA", msg) == 0 ) {
    // $GPGSA,A,3,,,,,,16,18,,22,24,,,3.6,2.1,2.2*3C
    // 2    = Mode: 1=Fix not available, 2=2D, 3=3D
    // 3-14 = IDs of SVs used in position fix (null for unused fields)
    // 15   = PDOP, 16   = HDOP, 17   = VDOP
    //
#ifdef DEBUG
    LOG( "MODE %s, PDOP %s, HDOP %s, VDOP %s", items[2], items[15], items[16], items[17] );
#endif
    if ( strcmp( "1", items[2]) == 0 ) {
      snprintf( ans, MAX_TEXT_LEN, "%s No fix", msg );
      gps_pos->fix = 0;
    } else if ( strcmp( "2", items[2]) == 0 ) {
      snprintf( ans, MAX_TEXT_LEN, "%s 2D fix", msg );
      gps_pos->fix = 1;
    } else if ( strcmp( "3", items[2]) == 0 ) {
      snprintf( ans, MAX_TEXT_LEN, "%s 3D fix", msg );
      gps_pos->fix = 1;
    }
  } else if ( strcmp("GPVTG", msg) == 0 ) {
    // $GPVTG,x.x,T,x.x,M,x.x,N,x.x,K,m,*hh
    // This is potential info for cloud/queue/HU
    // Example:
    //   $GPVTG,,T,0.0,M,0.0,N,0.0,K,A*0D
    // $GPVTG,t,T,,,s.ss,N,s.ss,K*hh
    // 1    = Track made good
    // 2    = Fixed text 'T' indicates that track made good is relative to true north
    // 3    = not used
    // 4    = not used
    // 5    = Speed over ground in knots
    // 6    = Fixed text 'N' indicates that speed over ground in in knots
    // 7    = Speed over ground in kilometers/hour
    // 8    = Fixed text 'K' indicates that speed over ground is in kilometers/hour
    // 9    = Checksum
    if ( strlen(items[1]) > 0 ) { // empty if no fix
      snprintf( ans, MAX_TEXT_LEN, "%s Track made good %s %s, speed %s %s",
		msg, items[1], items[2], items[7], items[8] );
      gps_pos->dir[PREVIOUS] = gps_pos->dir[CURRENT];
      gps_pos->dir[CURRENT]  = atof(items[1]);
      gps_pos->vel[PREVIOUS] = gps_pos->vel[CURRENT];
      gps_pos->vel[CURRENT]  = atof(items[7]);  // kmph
      gps_pos->fix = 1;
      gps_pos->updated = time_s();
    }
  } else if ( strcmp("GLGSV", msg) == 0 ) {
    // GLONASS, similar to GPGSV
    //   $GLGSV,3,1,10,66,14,195,29,86,28,092,26,75,02,073,24,87,,,23*52
    //   $GLGSV,3,2,10,85,26,029,19,67,48,244,17,68,33,320,15,78,08,258,*65
    //   $GLGSV,3,3,10,77,61,278,,76,49,063,*67
    //
    snprintf( ans, MAX_TEXT_LEN, "%s GLONASS satellites in view: %s", msg, items[3] );
  } else if ( strcmp("GNGSA", msg) == 0 ) {
    // Beidou(?) satellites, similar to GPGSA
    // Not observed.
    // Example:
    //   $GNGSA,A,1,,,,,,,,,,,,,,,
    if ( strcmp( "1", items[2]) == 0 ) {
      snprintf( ans, MAX_TEXT_LEN, "%s No fix", msg );
      gps_pos->fix = 0;
    } else if ( strcmp( "2", items[2]) == 0 ) {
      snprintf( ans, MAX_TEXT_LEN, "%s 2D fix", msg );
      gps_pos->fix = 1;
    } else if ( strcmp( "3", items[2]) == 0 ) {
      snprintf( ans, MAX_TEXT_LEN, "%s 3D fix", msg );
      gps_pos->fix = 1;
    }
  } else if ( strcmp("GNGNS", msg) == 0 ) {
    // GLONASS/GPS satellites in view
    // fields:
    // 6 	Mode indicator:
    // Variable character field with one character for each supported constellation.
    // First character is for GPS
    // Second character is for GLONASS
    // Subsequent characters will be added for new constellation
    // Each character will be one of the following:
    // N = No fix. Satellite system not used in position fix, or fix not valid
    // A = Autonomous. Satellite system used in non-differential mode in position fix
    // D = Differential (including all OmniSTAR services). Satellite system used 
    //     in differential mode in position fix
    // P = Precise. Satellite system used in precision mode. Precision mode is 
    //     defined as: no deliberate degradation (such as Selective Availability)
    //     and higher resolution code (P-code) is used to compute position fix
    // R = Real Time Kinematic. Satellite system used in RTK mode with fixed integers
    // F = Float RTK. Satellite system used in real time kinematic mode with 
    //     floating integer s
    // E = Estimated (dead reckoning) Mode
    // M = Manual Input Mode
    // S = Simulator Mode
    // 7 	Number of SVs in use, range 00–99
    if ( strncmp(items[6], "NN", 2) != 0 ) { // NN means no fix at all
      snprintf( ans, MAX_TEXT_LEN, "%s %s / %s", msg, items[6], items[7] );
      gps_pos->fix = 0;
    }
  } else { 
#ifdef DEBUG
    snprintf( ans, MAX_TEXT_LEN, "%s UNHANDLED: %s", msg, nmea );
#endif
  }

  // Free allocated memory by split()
  int i;
  for( i = 0; i < num_items; i++ ) {
    free( items[i] );
  }
 
  return 0;
}

/*!
  main opens GPS tty, requests a channel, starts reading/sending loop.

  If we start this from an /etc/init.d script, the GPS is most likely
  not ready yet, startup_sleep is supposed to ease the pain (but is not a long
  term solution).
 */
int main( int argc, char *argv[] ) {
  int  gps_fd;
  int  c;
  char *device_name = NULL;
  char buff[GPS_BUFFER];
  char ans[MAX_TEXT_LEN];
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

  struct gps_pos_t gps_pos = {0};

  while ( (c = getopt(argc, argv, "d:s:")) != -1) {
    switch (c) {
    case 'd':
      device_name = strdup(optarg);
      break;
    case 's':
      startup_sleep = atoi(optarg);
      break;
    default:
      LOG("?? getopt returned character code 0%o ??\n", c);
    }
  }

  LOG( "Cloudgps starting in %i", startup_sleep );

  // Zzz
  sleep( startup_sleep );

  // Maybe ended while waiting
  if ( ! loop ) {
    LOG( "Interrupted, exit" );
    return 1;
  }

  if ( ! device_name ) {
    device_name = strdup("/dev/ttyACM2");
  }
  
  LOG( "Opening %s", device_name );
  gps_fd = open( device_name, O_RDONLY | O_NOCTTY );
  if ( gps_fd == -1 ) {
    LOG( "Can not open device, errno %i (%s), EXIT", errno, strerror(errno) );
    free( device_name );
    return 1;
  }

  /*
    Initial server-client communication is through control socket.
  */
  int control_sock = nn_socket( AF_SP, NN_REQ );
  if ( control_sock < 0 ) {
    LOG( "Cannot open control socket, errno %i (%s), EXIT", errno, strerror(errno) );
    free( device_name );
    return 1;
  }
  res = nn_connect( control_sock, "ipc:///tmp/control.ipc" );
  if ( res < 0 ) {
    LOG( "Cannot connect to control socket, errno %i (%s), EXIT", errno, strerror(errno) );
    free( device_name );
    return 1;
  }

  // Request a comms channel
  char msg[MSG_SIZE];
  memset( msg, 0, sizeof(msg) );
  snprintf( msg, MSG_SIZE, "REQCHANNEL" );
  int bytes = nn_send( control_sock, msg, strlen(msg), 0 );
  if ( bytes < 0 ) {
    LOG( "Cannot send REQCHANNEL, errno %i (%s), EXIT", errno, strerror(errno) );
    free( device_name );
    return 1;
  }

  // Receive answer from hub
  memset( msg, 0, sizeof(msg) );
  bytes = nn_recv( control_sock, &msg, MSG_SIZE, 0 );
  if ( bytes < 0 ) {
    LOG( "Cannot receive REQCHANNEL, errno %i (%s), EXIT", errno, strerror(errno) );
    free( device_name );
    return 1;
  }
  printf( "RECEIVED %s\n", msg );
  nn_shutdown(control_sock, 0);

  if ( strcmp(msg, "NOCHANNEL") == 0 ) {
    LOG( "No channels available, EXIT" );
    free( device_name );
    return 1;
  }

  // The received msg contains the endpoint, connect to it.
  int sock = nn_socket(AF_SP, NN_PUSH);
  if ( sock < 0 ) {
    LOG( "Cannot open nn_socket, errno %i (%s), EXIT", errno, nn_strerror(errno) );
    free( device_name );
    return 1;
  }
  res = nn_connect(sock, msg);
  if ( res < 0 ) {
    LOG( "Cannot nn_connect, errno %i (%s), EXIT", errno, nn_strerror(errno) );
    free( device_name );
    return 1;
  }
  char *phy = strdup(msg);
  
  // At this point, we should have access to GPS and access to nano socket.
 
  // Read loop.
  // We get timeouts sometimes, which can be fixed by closing and
  // re-opening the tty. Closing, however, takes 30 seconds, 30 seconds
  // in which we get no data.
  // Pressing CTRL-C on command line and restarting is instantaneous.
  /*
    May 17 14:43:55 b2qt user.info gps[1763]: $GPRMC,,V,,,,,,,,,,N*53
    May 17 14:43:56 b2qt user.info gps[1763]: timeout
    May 17 14:43:56 b2qt user.info gps[1763]: Closing after timeout.
    May 17 14:44:26 b2qt user.info gps[1763]: Re-opening after timeout.
    May 17 14:44:26 b2qt user.info gps[1763]: Re-opened device.
    May 17 14:44:27 b2qt user.info gps[1763]: $GPGSV,2,1,06,33,,,34,39,,,34,...
  */
  //
  int max_timeout = MAX_TIMEOUT;
  int heartbeat = 60; // send pos every 60 seconds even if stationary
  LOG( "GPS and nanosocket communication established" );
  while ( loop ) {
 
    // Set timeout to 4.0 seconds and read.
    //
    FD_ZERO( &read_fds );
    FD_ZERO( &write_fds );
    FD_ZERO( &except_fds );
    FD_SET( gps_fd, &read_fds );
    timeout.tv_sec  = 4;
    timeout.tv_usec = 0;
    res = select( gps_fd+1, &read_fds, &write_fds, &except_fds, &timeout );
    
    if ( res == -1 ) {
      LOG( "select error %i", errno );
      loop = 0;
    } else if ( res == 0 ) {
      LOG( "select timeout" );
      if ( --max_timeout == 0 ) {
	loop = 0;
      } else { 
	// test -- re-init
	LOG( "Closing after timeout." );
	int cr = close( gps_fd );
	if ( cr == -1 ) {
	  LOG( "Cannot close device, errno %i (%s), EXIT", errno, strerror(errno) );
	  free( device_name );
	  return 1;
	}
	sleep(1);
	//
	LOG( "Re-opening after timeout." );
	gps_fd = open( device_name, O_RDONLY | O_NOCTTY  );
	if ( gps_fd == -1 ) {
	  LOG( "Cannot open device, errno %i (%s), EXIT", errno, strerror(errno) );
	  free( device_name );
	  return 1;
	}
	LOG( "Re-opened device." );
	// end test
      }
    } else { // data received
      memset( buff, 0, sizeof(buff) );
      len = read( gps_fd, buff, GPS_BUFFER );
      if ( len == -1 ) {
	LOG( "Cannot read from device, errno %i (%s), EXIT", errno, strerror(errno) );
	free( device_name );
	return 1;
      }
      buff[strcspn(buff, "\r\n")] = 0; // we get single 0A/LFs too, erase'm all
      if ( strlen(buff) > 0 ) { 
#ifdef DEBUG
	LOG( buff ); // spams log files
#endif
	max_timeout = MAX_TIMEOUT; // reset counter
	memset( ans, 0, sizeof(ans) );
	parse_nmea( buff, ans, &gps_pos );
	if ( strlen(ans) == 0 ) {
	  //snprintf( ans, MAX_TEXT_LEN, "nothing" );
	} else {
	  // LOG( "%i: %s", msg_num, ans ); // spams log files
	  // gps_pos_str( &gps_pos, ans );
	  // snprintf( msg, MSG_SIZE, "ANS %s", ans ); //destroys ans
	  // LOG( "%i: %s", msg_num, ans ); // spams log files}
	  //
	  memset( msg, 0, sizeof(msg) );
	  unsigned long id = time_s() - rand();

	  char msg_type[6];
	  strncpy(msg_type, ans, 5);
	  msg_type[5] = '\0';

	  if ( gps_pos.fix == 1) {
	    // We don't want to push position on every message, only on
	    // those that actually have position information.
	    if ( 
		(strncmp("GPGGA", msg_type, 5) == 0)
		||
		(strncmp("GPRMC", msg_type, 5) == 0)
		 ) {
	      // Create the JSON message as a string, move to cloud.
	      // Only push position if we have a fix, and the position is
	      // different from the previous position.
	      if ( gps_pos_changed(&gps_pos) || (time_s() % heartbeat == 0) ) {
		snprintf( msg, MSG_SIZE, "latlng=%.6f,%.6f;", gps_pos.lat[CURRENT], gps_pos.lon[CURRENT] );
		//printf( "%s\n", msg ); // spam stdout
		int bytes = nn_send(sock, msg, strlen(msg), 0);	      
		++msg_num;
	      }
	    } else if ( strncmp("GPVTG", msg_type, 5) == 0 ) {
	      if ( gps_pos_changed(&gps_pos) || (time_s() % heartbeat == 0) ) {
		// Velocity
		memset( msg, 0, sizeof(msg) );
		id = time_s() - rand();
		snprintf( msg, MSG_SIZE, "speed=%i;", (int)(gps_pos.vel[CURRENT]*10) );
		//printf( "%s\n", msg ); // spam stdout
		bytes = nn_send(sock, msg, strlen(msg), 0);
		++msg_num;
	      }
	    }
	  } // if fix == 1
	} // strlen(ans) > 0

      } // strlen(buff) > 0
    } // res select > 0
    
  } // while(loop)

  LOG( "Exit loop" );

  int cr = close( gps_fd ); // Takes 30 secs sometimes?
  if ( cr == -1 ) {
    LOG( "Cannot close device, errno %i (%s)", errno, strerror(errno) );
  }

  // Shutdown channel
  memset( msg, 0, sizeof(msg) );
  snprintf( msg, MSG_SIZE, "ENDCHANNEL %s", phy );
  bytes = nn_send( control_sock, msg, strlen(msg), 0 );
  if ( bytes < 0 ) {
    LOG( "Cannot send ENDCHANNEL, errno %i (%s), EXIT", errno, strerror(errno) );
    free( device_name );
    return 1;
  }

  free( device_name );
 
  LOG( "Exit program." );
  return 0;
}
 
/* ---- EOT ---------------------------------------------------------------- */
