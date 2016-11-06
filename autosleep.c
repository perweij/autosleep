/*

    autosleep.c

    See README about what it does.
    See LICENSE for licensing details.

    Per Weijnitz <per.weijnitz@gmail.com>


    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <X11/extensions/XInput.h>
#include <gdk/gdkx.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include "config.h"




// shared memory segment size
#define SEGSIZE 100
#define USAGE   "\n\nUsage: seconds keyboard_id /full/path/to/command args..."



// mouse coordinates
typedef struct _coords {
  int x;
  int y;
} Coords;



// some macros
#define DIE(...)         do{ fprintf(stderr, __VA_ARGS__); \
                             exit(EXIT_FAILURE); } while(0)
#define DIE_PERROR(...)  do{ fprintf(stderr, __VA_ARGS__); perror(0); \
                             exit(EXIT_FAILURE); } while(0)



// prototypes
int          get_mouse_pos(Display *display, Coords *coords);
int          check_for_mouse_events(Display *display);
XDeviceInfo* find_device(Display *display, XID id);
int          register_events(Display *dpy, XDeviceInfo *info);
int          detect_events(XID keyboard_id);
int          check_for_events_within_timelimit(int shmid,
                                               unsigned int timelimit_secs,
                                               XID keyboard_id,
                                               Display *display);
void         main_loop(int   shmid, unsigned int timelimit_secs,
                       char **suspend_cmd, XID keyboard_id,
                       Display *display);
  


/*
 * Function: get_mouse_pos 
 * -----------------------
 *   Retrieves the current mouse pointer position.
 *
 *   Inspired by https://ubuntuforums.org/showthread.php?t=562087
 *   (user: cwcentral).
 *
 *   display: the current display
 *   coords: a Coords struct in which to write the position.
 *
 *   returns: coords, same as input.
 */
int get_mouse_pos(Display *display, Coords *coords) {
  XEvent event;

  /* get info about current pointer position */
  (void)XQueryPointer(display, RootWindow(display, DefaultScreen(display)),
                      &event.xbutton.root, &event.xbutton.window,
                      &event.xbutton.x_root, &event.xbutton.y_root,
                      &event.xbutton.x, &event.xbutton.y,
                      &event.xbutton.state);
  
  coords->x = event.xbutton.x;
  coords->y = event.xbutton.y;
  
  return 1;
}



/*
 * Function: check_for_mouse_events
 * --------------------------------
 *   A non-threadsafe way to keep track of mouse position movements.
 *
 *   display: the current display
 *
 *   returns: 1 if a movement has happened, otherwise 0.
 */
int check_for_mouse_events(Display *display) {
  static Coords prev_coords;
  Coords coords;
    
  coords.x = 0;
  coords.y = 0;

  (void)get_mouse_pos(display, &coords);
  if(coords.x != prev_coords.x || coords.y != prev_coords.y) {
    prev_coords.x = coords.x;
    prev_coords.y = coords.y;
    return 1;
  }
  return 0;
}




/*
 * Function: find_device
 * ---------------------
 *   Find the XDevice matching the given id.
 *   The ID can be determined by the user by running "xinput -list".
 *
 *   display: the current Display.
 *   id: the ID of the keyboard to monitor for activity.
 *
 *   returns: the XDevice if found, otherwise NULL.
 */
XDeviceInfo* find_device(Display *display, XID id) {
  XDeviceInfo	*devices;
  XDeviceInfo   *found = NULL;
  int		 loop;
  int		 num_devices;

  devices = XListInputDevices(display, &num_devices);

  for(loop=0; loop<num_devices; loop++) {
    if (devices[loop].use >= IsXExtensionDevice && devices[loop].id == id) {
      found = &devices[loop];
      break;
    }
  }

  return found;
}



/*
 * Function: register_events
 * -------------------------
 *   Registers events we want to pick up.
 *   
 *   Acknowledgement: 
 *      Frederic Lepied, France. <Frederic.Lepied@sugix.frmug.org>
 *      https://anonscm.debian.org/cgit/pkg-xorg/app/xinput.git/tree/src/test.c
 *
 *   dpy:  current display.
 *   info: the keyboard XDevice info.
 *
 *   returns: the number of registered event types.
 */
int register_events(Display *dpy, XDeviceInfo *info)
{
  int		   number = 0;	/* number of events registered */
  XEventClass	   event_list[7];
  int		   i;
  XDevice         *device;
  Window           root_win;
  unsigned long	   screen;
  XInputClassInfo *ip;

  screen = DefaultScreen(dpy);
  root_win = RootWindow(dpy, screen);

  device = XOpenDevice(dpy, info->id);

  if (!device) {
    DIE("unable to open device\n");
  }

  if (device->num_classes > 0) {
    int typearg = 0;
    for (ip = device->classes, i=0; i<info->num_classes; ip++, i++) {
      switch (ip->input_class) {
      case KeyClass:
        DeviceKeyPress(device, typearg, event_list[number]); number++;
        DeviceKeyRelease(device, typearg, event_list[number]); number++;
        break;

      case ButtonClass:
        DeviceButtonPress(device, typearg, event_list[number]); number++;
        DeviceButtonRelease(device, typearg, event_list[number]); number++;
        break;

      case ValuatorClass:
        DeviceMotionNotify(device, typearg, event_list[number]); number++;
        break;

      default:
        fprintf(stderr, "unknown class\n");
        break;
      }
    }

    if (XSelectExtensionEvent(dpy, root_win, event_list, number)) {
      fprintf(stderr, "error selecting extended events\n");
      return 0;
    }
  }
  return number;
}




/*
 * Function: detect_events
 * -----------------------
 *   Blocks until a registered event happens, then returns its type.
 *
 *   display: the current display.
 *
 *   returns: the occured event's type.
 */
int detect_events(XID keyboard_id) {
  XDeviceInfo *info;
  XEvent       Event;

  Display *display = XOpenDisplay( NULL );
  if( !display ){ return 1; }

  info = find_device(display, keyboard_id);

  if (!info) {
    DIE("unable to find device '%d'\n", (int)keyboard_id);
  }
  if(! register_events(display, info)) {
    DIE("no event registered...\n");
  }
  
  setvbuf(stdout, NULL, _IOLBF, 0);
  XNextEvent(display, &Event); 
  return Event.type;
}



/*
 * Function: check_for_events_within_timelimit
 * -------------------------------------------
 *   Waits for at most timelimit_secs seconds waiting for an event.
 *   Its not very elegant, but we need to fight off zombie processes.
 *
 *   shmid:            a shared memory segment id.
 *   timelimit_secs:   the number of seconds to wait.
 *   keyboard_monitor: the keyboard id to monitor.
 *
 *   returns: 1 if an event has happened, otherwise 0.
 */
int check_for_events_within_timelimit(int shmid, unsigned int timelimit_secs,
                                      XID keyboard_id, Display *display) {
  char *segptr = NULL;
  
  pid_t pid = fork();
  if (pid == -1) {
    perror(0);
    exit(EXIT_FAILURE);
  }

  if((segptr = shmat(shmid, 0, 0)) == (void *)-1) {
    DIE_PERROR("shmat");
  }
  
  sprintf(segptr, "%s", "");

  int updated = 0;
  if (pid) {
    // parent
    signal(SIGCHLD, SIG_IGN); 
    unsigned int i = 0;
    for(i = 0; i < timelimit_secs; i++) {
      if(strnlen(segptr, SEGSIZE)) {
        updated = 1;
        break;
      }

      if(check_for_mouse_events(display)) {
        updated = 1;
        break;
      }
      
      sleep(1);
    }

    kill(pid, SIGTERM);
    shmdt(segptr);
        
  } else {
    /* child */
    if((segptr = shmat(shmid, 0, 0)) == (void *)-1) {
      DIE_PERROR("shmat");
    }

    int checkev = detect_events(keyboard_id);
    if(checkev != -1) {
      snprintf(segptr, SEGSIZE, "%d", checkev);
    }
    shmdt(segptr);    
    exit(0);
  }

  return updated;
}
         

        

/*
 * Function: main_loop
 * -------------------
 *  Waits for an event to happen within a timelimit. If it doesn't,
 *  the given command is executed. Then it loops and starts over again.
 *
 *  shmid:          a shared memory segment.
 *  timelimit_secs: a time limit.
 *  suspend_cmd:    a NULL terminated array of the command and arguments.
 *  keyboard_id:    the id of the keyboard to monitor.
 *  display:        the current display.
 *
 */
void main_loop(int   shmid, unsigned int timelimit_secs,
               char **suspend_cmd, XID keyboard_id, Display *display) {
  pid_t pid = 0;
  int   updated = 0;
  
  while(1) {
    updated = check_for_events_within_timelimit(shmid, timelimit_secs,
                                                keyboard_id, display);
    if(!updated) {
      pid = fork();
      if (pid == -1) {
        perror(0);
        _exit(EXIT_FAILURE);
      } else if (pid > 0) {
        // parent
        int status;
        waitpid(pid, &status, 0);
      } else {
        // child
        setsid();
        execv(suspend_cmd[0], (char * const*)suspend_cmd);
        perror("execv");
        _exit(EXIT_FAILURE);   // exec never returns
      }
    }
     
    sleep(1);
  }
}





int main(int argc, char *argv[]) {
  key_t          key;
  int            shmid = 0;
  XID            keyboard_id = 0;
  Display       *display = NULL;
  unsigned int   timelimit_secs = 0;
  char         **suspend_cmd = NULL;
  int            i = 0;
  
  // Do all the initiation here
  
  if(argc < 4) {
    DIE("wrong number of arguments %d %s. %s\n", argc, argv[1],
            USAGE);
    return 1;
  }

  timelimit_secs = (unsigned int)strtol(argv[1], NULL, 10);
  if (timelimit_secs == 0 && errno == EINVAL) {
    DIE("Error: supply time limit in seconds. %s\n", USAGE);
  }

  keyboard_id = (XID)strtol(argv[2], NULL, 10);
  if (keyboard_id == 0 && errno == EINVAL) {
    DIE("Error: supply keyboard ID (use xinput -list). %s\n",
            USAGE);
  }

  key = ftok(".", 'S');
  if (key == -1) {
    DIE("Error: could not generate a System V IPC key\n");
  }

  shmid = shmget( key, SEGSIZE, IPC_CREAT | 0660 );
  if(shmid == -1) {
    DIE("Error: could not get a shared mem segment\n");
  }

  display = XOpenDisplay( NULL );
  if( !display ) {
    DIE("Error: could not get the display\n");
  }

  if(access(argv[3], F_OK) == -1) {
    DIE("Error: supply full path to existing command. %s\n",
            USAGE);
  }

  /* prepare the suspend command */
  suspend_cmd = (char **)malloc(sizeof(char *) * argc - 1);
  if(suspend_cmd == NULL) {
    DIE("Error: could not allocate memory\n");
  }  
  for(i = 0; i < argc - 3; i++) {
    suspend_cmd[i] = argv[3+i];
  }
  suspend_cmd[i] = NULL;

  main_loop(shmid, timelimit_secs, suspend_cmd, keyboard_id, display);

  return 0;
}


/* eof */
