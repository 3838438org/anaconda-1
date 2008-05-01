/*
 * loader.c
 *
 * This is the installer loader.  Its job is to somehow load the rest
 * of the installer into memory and run it.  This may require setting
 * up some devices and networking, etc. The main point of this code is
 * to stay SMALL! Remember that, live by that, and learn to like it.
 *
 * Copyright (C) 1997, 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005,
 * 2006, 2007  Red Hat, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author(s): Erik Troan <ewt@redhat.com>
 *            Matt Wilson <msw@redhat.com>
 *            Michael Fulbright <msf@redhat.com>
 *            Jeremy Katz <katzj@redhat.com>
 */

#include <ctype.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <newt.h>
#include <popt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <unistd.h>
#include <stdint.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <linux/fb.h>
#include <linux/serial.h>
#include <linux/vt.h>

#ifdef USE_MTRACE
#include <mcheck.h>
#endif

#include "copy.h"
#include "getparts.h"
#include "loader.h"
#include "loadermisc.h" /* JKFIXME: functions here should be split out */
#include "log.h"
#include "lang.h"
#include "fwloader.h"
#include "kbd.h"
#include "kickstart.h"
#include "windows.h"

/* module stuff */
#include "modules.h"
#include "moduleinfo.h"

#include "driverdisk.h"

/* hardware stuff */
#include "hardware.h"

/* install method stuff */
#include "method.h"
#include "cdinstall.h"
#include "nfsinstall.h"
#include "hdinstall.h"
#include "urlinstall.h"

#include "net.h"
#include "telnetd.h"

#include <selinux/selinux.h>
#include "selinux.h"

#include "../isys/imount.h"
#include "../isys/isys.h"
#include "../isys/stubs.h"
#include "../isys/lang.h"
#include "../isys/eddsupport.h"
#include "../isys/str.h"

/* maximum number of extra arguments that can be passed to the second stage */
#define MAX_EXTRA_ARGS 128
static char * extraArgs[MAX_EXTRA_ARGS];
static int hasGraphicalOverride();

static int newtRunning = 0;

/* boot flags -- we need these in a lot of places */
uint64_t flags = LOADER_FLAGS_SELINUX;

#ifdef INCLUDE_LOCAL
#include "cdinstall.h"
#include "hdinstall.h"
#endif
#ifdef INCLUDE_NETWORK
#include "nfsinstall.h"
#include "urlinstall.h"
#endif

int num_link_checks = 5;
int post_link_sleep = 0;

static struct installMethod installMethods[] = {
#if !defined(__s390__) && !defined(__s390x__)
    { N_("Local CD/DVD"), 0, DEVICE_CDROM, mountCdromImage },
#endif
    { N_("Hard drive"), 0, DEVICE_DISK, mountHardDrive },
    { N_("NFS directory"), 1, DEVICE_NETWORK, mountNfsImage },
    { "URL", 1, DEVICE_NETWORK, mountUrlImage },
};
static int numMethods = sizeof(installMethods) / sizeof(struct installMethod);

void doSuspend(void) {
    newtFinished();
    exit(1);
}

void doShell(void) {
    /* this lets us debug the loader just by having a second initramfs
     * containing /sbin/busybox */
    int child, status;

    newtSuspend();
    if (!(child = fork())) {
	    execl("/sbin/busybox", "msh", NULL);
	    _exit(1);
    }
    waitpid(child, &status, 0);
    newtResume();
}

void startNewt(void) {
    if (!newtRunning) {
        char *buf;
        int ignore;
        char *arch = getProductArch();
        
        ignore = asprintf(&buf, _("Welcome to %s for %s"), getProductName(),
                arch);

        newtInit();
        newtCls();
        newtDrawRootText(0, 0, buf);
        free(buf);
        
        newtPushHelpLine(_("  <Tab>/<Alt-Tab> between elements  | <Space> selects | <F12> next screen "));
        
        newtRunning = 1;
        if (FL_TESTING(flags)) 
            newtSetSuspendCallback((void *) doSuspend, NULL);
        else if (!access("/sbin/busybox",  X_OK)) 
            newtSetSuspendCallback((void *) doShell, NULL);
    }
}

void stopNewt(void) {
    if (newtRunning) newtFinished();
    newtRunning = 0;
}

static char * productName = NULL;
static char * productPath = NULL;
static char * productArch = NULL;
static char * productStamp = NULL;

static void initProductInfo(void) {
    FILE *f;
    int i;

    f = fopen("/.buildstamp", "r");
    if (!f) {
        productName = strdup("anaconda");
        productPath = strdup("anaconda");
    } else {
        productStamp = malloc(256);
        productName = malloc(256);
        productPath = malloc(256);
        productStamp = fgets(productStamp, 256, f); /* stamp time and architecture */
        productArch = strstr(productStamp, "."); /* architecture is separated by dot */
        if(productArch) productArch++;

        productName = fgets(productName, 256, f); /* product name */
        productPath = fgets(productPath, 256, f); /* product version */
        productPath = fgets(productPath, 256, f); /* product path */

        i = strlen(productName) - 1;
        while (isspace(*(productName + i))) {
            *(productName + i) = '\0';
            i--;
        }
        i = strlen(productPath) - 1;
        while (isspace(*(productPath + i))) {
            *(productPath + i) = '\0';
            i--;
        }
        i = strlen(productArch) - 1;
        while (isspace(*(productArch + i))) {
            *(productArch + i) = '\0';
            i--;
        }
    }

    if(!productArch) productArch = strdup("unknown architecture");

    fclose(f);
}

char * getProductName(void) {
    if (!productName) {
       initProductInfo();
    }
    return productName;
}

char * getProductArch(void) {
    if (!productArch) {
       initProductInfo();
    }
    return productArch;
}

char * getProductPath(void) {
    if (!productPath) {
       initProductInfo();
    }
    return productPath;
}

void initializeConsole() {
    /* enable UTF-8 console */
    printf("\033%%G");
    fflush(stdout);

    isysLoadFont();
    if (!FL_TESTING(flags))
        isysSetUnicodeKeymap();
}

/* fbcon is buggy and resets our color palette if we allocate a terminal
 * after initializing it, so we initialize 9 of them before we need them.
 * If it doesn't work, the user gets to suffer through having an ugly palette,
 * but things are still usable. */
static void initializeTtys(void) {
    int fd, n;
    char dev[] = "/dev/ttyX";

    for (n = 9; n > 0; n--) {
	sprintf(dev, "/dev/tty%d", n);
	mknod(dev, 0600 | S_IFCHR, makedev(4, n));
	fd = open(dev, O_RDWR|O_NOCTTY);
	if (fd >= 0) {
	    ioctl(fd, VT_ACTIVATE, n);
	    if (n == 1)
		ioctl(fd, VT_WAITACTIVE, n);
	    close(fd);
	} else
	    logMessage(ERROR, "failed to initialize %s", dev);
    }
}

static void spawnShell(void) {
    pid_t pid;

    if (FL_SERIAL(flags) || FL_NOSHELL(flags)) {
        logMessage(INFO, "not spawning a shell");
        return;
    } else if (access("/bin/sh",  X_OK))  {
        logMessage(ERROR, "cannot open shell - /bin/sh doesn't exist");
        return;
    }

    if (!(pid = fork())) {
	int fd;

    	fd = open("/dev/tty2", O_RDWR|O_NOCTTY);
    	if (fd < 0) {
            logMessage(ERROR, "cannot open /dev/tty2 -- no shell will be provided");
	    return;
	}

        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        
        close(fd);
        setsid();

	/* enable UTF-8 console */
	printf("\033%%G");
	fflush(stdout);
	isysLoadFont();
	
        if (ioctl(0, TIOCSCTTY, NULL)) {
            logMessage(ERROR, "could not set new controlling tty");
        }
        
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);

        if (!access("/mnt/source/RHupdates/pyrc.py", R_OK|X_OK))
            setenv("PYTHONSTARTUP", "/mnt/source/RHupdates/pyrc.py", 1);
        else if (!access("/tmp/updates/pyrc.py", R_OK|X_OK))
            setenv("PYTHONSTARTUP", "/tmp/updates/pyrc.py", 1);
        else if (!access("/usr/lib/anaconda-runtime/pyrc.py", R_OK|X_OK))
            setenv("PYTHONSTARTUP", "/usr/lib/anaconda-runtime/pyrc.py", 1);
        setenv("LD_LIBRARY_PATH", LIBPATH, 1);
        setenv("LANG", "C", 1);
        
        if (execl("/bin/sh", "-/bin/sh", NULL) == -1) {
            logMessage(CRITICAL, "exec of /bin/sh failed: %s", strerror(errno));
            exit(1);
        }
    }

    return;
}


static void copyWarnFn (char *msg) {
   logMessage(WARNING, msg);
}

static void copyErrorFn (char *msg) {
   newtWinMessage(_("Error"), _("OK"), _(msg));
}

void loadUpdates(struct loaderData_s *loaderData) {
    char *device = NULL, *part = NULL, *buf;
    char **devNames = NULL;
    enum { UPD_DEVICE, UPD_PART, UPD_LOAD, UPD_DONE } stage = UPD_DEVICE;
    int rc, num = 0;
    int dir = 1;

    while (stage != UPD_DONE) {
        switch (stage) {
        case UPD_DEVICE: {
            rc = getRemovableDevices(&devNames);
            if (rc == 0)
                return;

            /* we don't need to ask which to use if they only have one */
            if (rc == 1) {
                device = strdup(devNames[0]);
                free(devNames);
                if (dir == -1)
                    return;

                stage = UPD_PART;
                break;
            }
            dir = 1;

            startNewt();
            rc = newtWinMenu(_("Update Disk Source"),
                             _("You have multiple devices which could serve "
                               "as sources for an update disk.  Which would "
                               "you like to use?"), 40, 10, 10,
                             rc < 6 ? rc : 6, devNames,
                             &num, _("OK"), _("Cancel"), NULL);

            if (rc == 2) {
                free(devNames);
                return;
            }

            device = strdup(devNames[num]);
            free(devNames);
            stage = UPD_PART;
        }

        case UPD_PART: {
            char ** part_list = getPartitionsList(device);
            int nump = 0, num = 0;

            if (part != NULL) free(part);

            if ((nump = lenPartitionsList(part_list)) == 0) {
                if (dir == -1)
                    stage = UPD_DEVICE;
                else {
                    part = device;
                    stage = UPD_LOAD;
                }

                break;
            }
            dir = 1;

            startNewt();
            rc = newtWinMenu(_("Update Disk Source"),
                             _("There are multiple partitions on this device "
                               "which could contain the update disk image.  "
                               "Which would you like to use?"), 40, 10, 10,
                             nump < 6 ? nump : 6, part_list, &num, _("OK"),
                             _("Back"), NULL);

            if (rc == 2) {
                freePartitionsList(part_list);
                stage = UPD_DEVICE;
                dir = -1;
                break;
            }

            part = strdup(part_list[num]);
            stage = UPD_LOAD;
        }

        case UPD_LOAD:
            rc = asprintf(&buf, _("Insert your updates disk into /dev/%s and "
                                  "press \"OK\" to continue."), part+5);
            rc = newtWinChoice(_("Updates Disk"), _("OK"), _("Back"), buf);
            free(buf);

            if (rc == 2) {
                stage = UPD_PART;
                dir = -1;
                break;
            }

            logMessage(INFO, "UPDATES device is %s", part);

            if (doPwMount(part, "/tmp/update-disk", "ext2", "ro") &&
                doPwMount(part, "/tmp/update-disk", "vfat", "ro") &&
                doPwMount(part, "/tmp/update-disk", "iso9660", "ro")) {
                newtWinMessage(_("Error"), _("OK"),
                               _("Failed to mount updates disk"));
            } else {
                /* Copy everything to /tmp/updates so we can unmount the disk  */
                winStatus(40, 3, _("Updates"), _("Reading anaconda updates..."));
                if (!copyDirectory("/tmp/update-disk", "/tmp/updates", copyWarnFn,
                                   copyErrorFn)) {
                    dir = 1;
                    stage = UPD_DONE;
                }

                newtPopWindow();
                umount("/tmp/update-disk");
            }

        case UPD_DONE:
            break;
        }
    }

    return;
}

static char *newUpdatesLocation(const char *origLocation) {
    const char *location;
    char *retval = NULL;
    newtComponent f, okay, cancel, answer, locationEntry;
    newtGrid grid, buttons;

    startNewt();

    locationEntry = newtEntry(-1, -1, NULL, 60, &location, NEWT_FLAG_SCROLL);
    newtEntrySet(locationEntry, origLocation, 1);

    /* button bar at the bottom of the window */
    buttons = newtButtonBar(_("OK"), &okay, _("Cancel"), &cancel, NULL);

    grid = newtCreateGrid(1, 3);

    newtGridSetField(grid, 0, 0, NEWT_GRID_COMPONENT,
                     newtTextboxReflowed(-1, -1, _("Unable to download the updates image.  Please modify the updates location below or press Cancel to proceed without updates.."), 60, 0, 0, 0),
                     0, 0, 0, 0, NEWT_ANCHOR_LEFT, 0);
    newtGridSetField(grid, 0, 1, NEWT_GRID_COMPONENT, locationEntry,
                     0, 1, 0, 0, NEWT_ANCHOR_LEFT, 0);
    newtGridSetField(grid, 0, 2, NEWT_GRID_SUBGRID, buttons,
                     0, 1, 0, 0, 0, NEWT_GRID_FLAG_GROWX);

    f = newtForm(NULL, NULL, 0);
    newtGridAddComponentsToForm(grid, f, 1);
    newtGridWrappedWindow(grid, _("Error downloading updates image"));
    newtGridFree(grid, 1);

    /* run the form */
    answer = newtRunForm(f);

    if (answer != cancel)
        retval = strdup(location);

    newtFormDestroy(f);
    newtPopWindow();

    return retval;
}

static int loadUpdatesFromRemote(char * url, struct loaderData_s * loaderData) {
    int rc = getFileFromUrl(url, "/tmp/updates.img", loaderData);

    if (rc != 0) {
        char *newLocation = newUpdatesLocation(url);

        if (!newLocation)
           return rc;
        else
           return loadUpdatesFromRemote(newLocation, loaderData);
    }

    copyUpdatesImg("/tmp/updates.img");
    unlink("/tmp/updates.img");
    return 0;
}

static void writeVNCPasswordFile(char *pfile, char *password) {
    FILE *f;

    f = fopen(pfile, "w+");
    fprintf(f, "%s\n", password);
    fclose(f);
}

/* read information from /tmp/netinfo (written by linuxrc) */
static void readNetInfo(struct loaderData_s ** ld) {
    int i;
    struct loaderData_s * loaderData = *ld;
    FILE *f;
    /* FIXME: arbitrary size that works, but could blow up in the future */
    int bufsiz = 100;
    char buf[bufsiz], *vname, *vparm;

    f = fopen("/tmp/netinfo", "r");
    if (!f)
        return;

    /* FIXME: static buffers lead to pain */
    vname = (char *)malloc(sizeof(char)*15);
    vparm = (char *)malloc(sizeof(char)*85);

    /* make sure everything is NULL before we begin copying info */
    loaderData->ipv4 = NULL;
    loaderData->netmask = NULL;
    loaderData->gateway = NULL;
    loaderData->dns = NULL;
    loaderData->peerid = NULL;
    loaderData->subchannels = NULL;
    loaderData->portname = NULL;
    loaderData->nettype = NULL;
    loaderData->ctcprot = NULL;
    loaderData->layer2 = NULL;
    loaderData->macaddr = NULL;

    /*
     * The /tmp/netinfo file is written out by /sbin/init on s390x (which is
     * really the linuxrc.s390 script).  It's a shell-sourcable file with
     * various system settings needing for the system instance.
     *
     * The goal of this function is to read in only the network settings
     * and populate the loaderData structure.
     */
    while(fgets(buf, bufsiz, f)) {
        /* trim whitespace from end */
        i = 0;
        while (!isspace(buf[i]) && i < (bufsiz-1))
            i++;
        buf[i] = '\0';

        /* break up var name and value */
        if (strstr(buf, "=")) {
            vname = strtok(buf, "=");
            if (vname == NULL)
                continue;

            vparm = strtok(NULL, "=");
            if (vparm == NULL)
                continue;

            if (!strncmp(vname, "IPADDR", 6))
                loaderData->ipv4 = strdup(vparm);

            if (!strncmp(vname, "NETMASK", 7))
                loaderData->netmask = strdup(vparm);

            if (!strncmp(vname, "GATEWAY", 7))
                loaderData->gateway = strdup(vparm);

            if (!strncmp(vname, "DNS", 3))
                loaderData->dns = strdup(vparm);

            if (!strncmp(vname, "MTU", 3))
                loaderData->mtu = atoi(vparm);

            if (!strncmp(vname, "PEERID", 6))
                loaderData->peerid = strdup(vparm);

            if (!strncmp(vname, "SUBCHANNELS", 12))
                loaderData->subchannels = strdup(vparm);

            if (!strncmp(vname, "PORTNAME", 8))
                loaderData->portname = strdup(vparm);

            if (!strncmp(vname, "NETTYPE", 7))
                loaderData->nettype = strdup(vparm);

            if (!strncmp(vname, "CTCPROT", 7))
                loaderData->ctcprot = strdup(vparm);

            if (!strncmp(vname, "LAYER2", 6))
                loaderData->layer2 = strdup(vparm);

            if (!strncmp(vname, "MACADDR", 7))
                loaderData->macaddr = strdup(vparm);

            if (!strncmp(vname, "HOSTNAME", 8))
                loaderData->hostname = strdup(vparm);
        }
    }

    if (loaderData->ipv4 && loaderData->netmask)
        flags |= LOADER_FLAGS_HAVE_CMSCONF;

    fclose(f);
}

/* parse anaconda or pxelinux-style ip= arguments
 * pxelinux format: ip=<client-ip>:<boot-server-ip>:<gw-ip>:<netmask>
 * anaconda format: ip=<client-ip> netmask=<netmask> gateway=<gw-ip>
*/
static void parseCmdLineIp(struct loaderData_s * loaderData, char *argv)
{
    /* Detect pxelinux */
    if (strstr(argv, ":") != NULL) {
        char *start, *end;

        /* IP */
        start = argv + 3;
        end = strstr(start, ":");
        loaderData->ipv4 = strndup(start, end-start);
        loaderData->ipinfo_set = 0;

        /* Boot server */
        if (end + 1 == '\0')
            return;
        start = end + 1;
        end = strstr(start, ":");
        if (end == NULL)
            return;

        /* Gateway */
        if (end + 1 == '\0')
            return;
        start = end + 1;
        end = strstr(start, ":");
        if (end == NULL) {
            loaderData->gateway = strdup (start);
            return;
        } else {
            loaderData->gateway = strndup(start, end-start);
        }

        /* Netmask */
        if (end + 1 == '\0')
            return;
        start = end + 1;
        loaderData->netmask = strdup(start);
    } else {
        loaderData->ipv4 = strdup(argv + 3);
        loaderData->ipinfo_set = 0;
    }

    if (loaderData->ipinfo_set || !strncmp(loaderData->ipv4, "dhcp", 4))
        flags |= LOADER_FLAGS_IP_PARAM;
}

/*
 * parse anaconda ipv6= arguments
 */
static void parseCmdLineIpv6(struct loaderData_s * loaderData, char *argv)
{
    /* right now we only accept ipv6= arguments equal to:
     *     dhcp     DHCPv6 call
     *     auto     RFC 2461 neighbor discovery
     */
    loaderData->ipv6 = NULL;

    if (!strncmp(str2lower(argv), "ipv6=dhcp", 9)) {
        loaderData->ipv6 = strdup("dhcp");
    } else if (!strncmp(str2lower(argv), "ipv6=auto", 9)) {
        loaderData->ipv6 = strdup("auto");
    }

    if (loaderData->ipv6 != NULL) {
        loaderData->ipv6info_set = 1;
        flags |= LOADER_FLAGS_IPV6_PARAM;
    }

    return;
}

/* parses /proc/cmdline for any arguments which are important to us.  
 * NOTE: in test mode, can specify a cmdline with --cmdline
 */
static void parseCmdLineFlags(struct loaderData_s * loaderData,
                              char * cmdLine) {
    int fd;
    char buf[1024];
    int len;
    char ** argv;
    int argc;
    int numExtraArgs = 0;
    int i;
    char *front;

    /* we want to default to graphical and allow override with 'text' */
    flags |= LOADER_FLAGS_GRAPHICAL;

    /* if we have any explicit cmdline (probably test mode), we don't want
     * to parse /proc/cmdline */
    if (!cmdLine) {
        if ((fd = open("/proc/cmdline", O_RDONLY)) < 0) return;
        len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len <= 0) {
            logMessage(INFO, "kernel command line was empty");
            return;
        }
        
        buf[len] = '\0';
        cmdLine = buf;
    }

    logMessage(INFO, "kernel command line: %s", cmdLine);
    
    if (poptParseArgvString(cmdLine, &argc, (const char ***) &argv))
        return;

    for (i=0; i < argc; i++) {
        if (!strcasecmp(argv[i], "expert")) {
            flags |= LOADER_FLAGS_EXPERT;
            logMessage(INFO, "expert got used, ignoring");
            /* flags |= (LOADER_FLAGS_EXPERT | LOADER_FLAGS_MODDISK | 
                        LOADER_FLAGS_ASKMETHOD);*/
        } else if (!strcasecmp(argv[i], "askmethod"))
            flags |= LOADER_FLAGS_ASKMETHOD;
        else if (!strcasecmp(argv[i], "asknetwork"))
            flags |= LOADER_FLAGS_ASKNETWORK;
        else if (!strcasecmp(argv[i], "noshell"))
            flags |= LOADER_FLAGS_NOSHELL;
        else if (!strcasecmp(argv[i], "mediacheck"))
            flags |= LOADER_FLAGS_MEDIACHECK;
        else if (!strcasecmp(argv[i], "allowwireless"))
            flags |= LOADER_FLAGS_ALLOW_WIRELESS;
        else if (!strcasecmp(argv[i], "telnet"))
            flags |= LOADER_FLAGS_TELNETD;
        else if (!strcasecmp(argv[i], "noprobe"))
            flags |= LOADER_FLAGS_NOPROBE;
        else if (!strcasecmp(argv[i], "text")) {
            logMessage(INFO, "text mode forced from cmdline");
            flags |= LOADER_FLAGS_TEXT;
            flags &= ~LOADER_FLAGS_GRAPHICAL;
        }
        else if (!strcasecmp(argv[i], "graphical")) {
            logMessage(INFO, "graphical mode forced from cmdline");
            flags |= LOADER_FLAGS_GRAPHICAL;
        } else if (!strcasecmp(argv[i], "cmdline")) {
            logMessage(INFO, "cmdline mode forced from cmdline");
            flags |= LOADER_FLAGS_CMDLINE;
        } else if (!strncasecmp(argv[i], "updates=", 8))
            loaderData->updatessrc = strdup(argv[i] + 8);
        else if (!strncasecmp(argv[i], "updates", 7))
            flags |= LOADER_FLAGS_UPDATES;
        else if (!strncasecmp(argv[i], "dogtail=", 8))
            loaderData->dogtailurl = strdup(argv[i] + 8);
        else if (!strncasecmp(argv[i], "dd=", 3) || 
                 !strncasecmp(argv[i], "driverdisk=", 11)) {
            loaderData->ddsrc = strdup(argv[i] + 
                                       (argv[i][1] == 'r' ? 11 : 3));
        }
        else if (!strcasecmp(argv[i], "dd") || 
                 !strcasecmp(argv[i], "driverdisk"))
            flags |= LOADER_FLAGS_MODDISK;
        else if (!strcasecmp(argv[i], "rescue"))
            flags |= LOADER_FLAGS_RESCUE;
        else if (!strcasecmp(argv[i], "nopass"))
            flags |= LOADER_FLAGS_NOPASS;
        else if (!strcasecmp(argv[i], "serial")) 
            flags |= LOADER_FLAGS_SERIAL;
        else if (!strcasecmp(argv[i], "noipv4"))
            flags |= LOADER_FLAGS_NOIPV4;
        else if (!strcasecmp(argv[i], "noipv6"))
            flags |= LOADER_FLAGS_NOIPV6;
        else if (!strcasecmp(argv[i], "kssendmac"))
            flags |= LOADER_FLAGS_KICKSTART_SEND_MAC;
        /* deprecated hardware bits */
        else if (!strcasecmp(argv[i], "nousbstorage"))
            mlAddBlacklist("usb-storage");
        else if (!strcasecmp(argv[i], "nousb")) {
            mlAddBlacklist("ehci-hcd");
            mlAddBlacklist("ohci-hcd");
            mlAddBlacklist("uhci-hcd");
        } else if (!strcasecmp(argv[i], "nofirewire"))
            mlAddBlacklist("firewire-ohci");
        else if (!strncasecmp(argv[i], "loglevel=", 9)) {
            if (!strcasecmp(argv[i]+9, "debug")) {
                loaderData->logLevel = strdup(argv[i]+9);
                setLogLevel(DEBUGLVL);
            }
            else if (!strcasecmp(argv[i]+9, "info")) {
                loaderData->logLevel = strdup(argv[i]+9);
                setLogLevel(INFO);
            }
            else if (!strcasecmp(argv[i]+9, "warning")) {
                loaderData->logLevel = strdup(argv[i]+9);
                setLogLevel(WARNING);
            }
            else if (!strcasecmp(argv[i]+9, "error")) {
                loaderData->logLevel = strdup(argv[i]+9);
                setLogLevel(ERROR);
            }
            else if (!strcasecmp(argv[i]+9, "critical")) {
                loaderData->logLevel = strdup(argv[i]+9);
                setLogLevel(CRITICAL);
            }
        }
        else if (!strncasecmp(argv[i], "ksdevice=", 9)) {
            loaderData->netDev = strdup(argv[i] + 9);
            loaderData->netDev_set = 1;
        }
        else if (!strncmp(argv[i], "BOOTIF=", 7)) {
            /* +10 so that we skip over the leading 01- */
            loaderData->bootIf = strdup(argv[i] + 10);

            /* scan the BOOTIF value and replace '-' with ':' */
            front = loaderData->bootIf;
            if (front) {
                while (*front != '\0') {
                    if (*front == '-')
                        *front = ':';
                    front++;
                }
            }

            loaderData->bootIf_set = 1;
        } else if (!strncasecmp(argv[i], "dhcpclass=", 10)) {
            loaderData->netCls = strdup(argv[i] + 10);
            loaderData->netCls_set = 1;
        }
        else if (!strcasecmp(argv[i], "ks") || !strncasecmp(argv[i], "ks=", 3))
            loaderData->ksFile = strdup(argv[i]);
        else if (!strncasecmp(argv[i], "display=", 8))
            setenv("DISPLAY", argv[i] + 8, 1);
        else if ((!strncasecmp(argv[i], "lang=", 5)) && 
                 (strlen(argv[i]) > 5))  {
            loaderData->lang = strdup(argv[i] + 5);
            loaderData->lang_set = 1;
        }
        else if (!strncasecmp(argv[i], "keymap=", 7) &&
                   (strlen(argv[i]) > 7)) {
            loaderData->kbd = strdup(argv[i] + 7);
            loaderData->kbd_set = 1;
        }
        else if (!strncasecmp(argv[i], "method=", 7) && !FL_STAGE2(flags))
            setMethodFromCmdline(argv[i] + 7, loaderData);
        else if (!strncasecmp(argv[i], "stage2=", 7)) {
            flags |= LOADER_FLAGS_STAGE2;
            setMethodFromCmdline(argv[i] + 7, loaderData);
        }
        else if (!strncasecmp(argv[i], "hostname=", 9))
            loaderData->hostname = strdup(argv[i] + 9);
        else if (!strncasecmp(argv[i], "ip=", 3))
            parseCmdLineIp(loaderData, argv[i]);
        else if (!strncasecmp(argv[i], "ipv6=", 5))
            parseCmdLineIpv6(loaderData, argv[i]);
        else if (!strncasecmp(argv[i], "netmask=", 8)) 
            loaderData->netmask = strdup(argv[i] + 8);
        else if (!strncasecmp(argv[i], "gateway=", 8))
            loaderData->gateway = strdup(argv[i] + 8);
        else if (!strncasecmp(argv[i], "dns=", 4))
            loaderData->dns = strdup(argv[i] + 4);
        else if (!strncasecmp(argv[i], "ethtool=", 8))
            loaderData->ethtool = strdup(argv[i] + 8);
        else if (!strncasecmp(argv[i], "essid=", 6))
            loaderData->essid = strdup(argv[i] + 6);
        else if (!strncasecmp(argv[i], "mtu=", 4))
            loaderData->mtu = atoi(argv[i] + 4);
        else if (!strncasecmp(argv[i], "wepkey=", 7))
            loaderData->wepkey = strdup(argv[i] + 7);
        else if (!strncasecmp(argv[i], "linksleep=", 10))
            num_link_checks = atoi(argv[i] + 10);
        else if (!strncasecmp(argv[i], "nicdelay=", 9))
            post_link_sleep = atoi(argv[i] + 9);
        else if (!strncasecmp(argv[i], "dhcptimeout=", 12))
            loaderData->dhcpTimeout = atoi(argv[i] + 12);
        else if (!strncasecmp(argv[i], "selinux=0", 9))
            flags &= ~LOADER_FLAGS_SELINUX;
        else if (!strncasecmp(argv[i], "selinux", 7))
            flags |= LOADER_FLAGS_SELINUX;
        else if (numExtraArgs < (MAX_EXTRA_ARGS - 1)) {
            /* go through and append args we just want to pass on to */
            /* the anaconda script, but don't want to represent as a */
            /* LOADER_FLAGS_XXX since loader doesn't care about these */
            /* particular options.                                   */
            /* do vncpassword case first */
            if (!strncasecmp(argv[i], "vncpassword=", 12)) {
                if (!FL_TESTING(flags))
                    writeVNCPasswordFile("/tmp/vncpassword.dat", argv[i]+12);
            }
            else if (!strncasecmp(argv[i], "resolution=", 11) ||
                     !strncasecmp(argv[i], "lowres", 6) ||
                     !strncasecmp(argv[i], "nomount", 7) ||
                     !strncasecmp(argv[i], "vnc", 3) ||
                     !strncasecmp(argv[i], "vncconnect=", 11) ||
                     !strncasecmp(argv[i], "headless", 8) ||
                     !strncasecmp(argv[i], "usefbx", 6) ||
                     !strncasecmp(argv[i], "mpath", 6) ||
                     !strncasecmp(argv[i], "nompath", 8) ||
                     !strncasecmp(argv[i], "dmraid", 6) ||
                     !strncasecmp(argv[i], "nodmraid", 8) ||
                     !strncasecmp(argv[i], "xdriver=", 8) ||
                     !strncasecmp(argv[i], "vesa", 4) ||
                     !strncasecmp(argv[i], "syslog=", 7)) { 

                /* vnc implies graphical */
                if (!strncasecmp(argv[i], "vnc", 3)) {
                    logMessage(INFO, "vnc forced cmdline mode from cmdline");
                    flags |= LOADER_FLAGS_GRAPHICAL;
                }

                if (!strncasecmp(argv[i], "vesa", 4)) {
                    if (asprintf(&extraArgs[numExtraArgs],
                                 "--xdriver=vesa") == -1)
                        return;
                    logMessage(WARNING, "\"vesa\" command line argument is deprecated.  use \"xdriver=vesa\".");
                } else {
                    if (asprintf(&extraArgs[numExtraArgs],"--%s",argv[i]) == -1)
                        return;
                }
                numExtraArgs += 1;

                if (numExtraArgs > (MAX_EXTRA_ARGS - 2)) {
                     logMessage(WARNING, "Too many command line arguments (max "
                                "allowed is %d), rest will be dropped.",
                                MAX_EXTRA_ARGS);
                }
            }
        }
    }

    readNetInfo(&loaderData);

    /* NULL terminates the array of extra args */
    extraArgs[numExtraArgs] = NULL;

    return;
}


#if 0
/* determine if we are using a framebuffer console.  return 1 if so */
static int checkFrameBuffer() {
    int fd;
    int rc = 0;
    struct fb_fix_screeninfo fix;

    if ((fd = open("/dev/fb0", O_RDONLY)) == -1) {
        return 0;
    }
    
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) >= 0) {
        rc = 1;
    }
    close(fd);
    return rc;
}
#endif


/* make sure they have enough ram */
static void checkForRam(void) {
    if (totalMemory() < MIN_RAM) {
        char *buf;
        int i;
        i = asprintf(&buf, _("You do not have enough RAM to install %s "
                             "on this machine."), getProductName());
        startNewt();
        newtWinMessage(_("Error"), _("OK"), buf);
        free(buf);
        stopNewt();
        exit(0);
    }
}

static int haveDeviceOfType(int type) {
    struct device ** devices;

    devices = getDevices(type);
    if (devices) {
        return 1;
    }
    return 0;
}

/* fsm for the basics of the loader. */
static char *doLoaderMain(char * location,
                          struct loaderData_s * loaderData,
                          moduleInfoSet modInfo) {
    enum { STEP_LANG, STEP_KBD, STEP_METHOD, STEP_DRIVER, 
           STEP_DRIVERDISK, STEP_NETWORK, STEP_IFACE,
           STEP_IP, STEP_URL, STEP_DONE } step;
    char * url = NULL;
    char * ret = NULL;
    int dir = 1;
    int rc, i;

    char * installNames[10]; /* 10 install methods will be enough for anyone */
    int numValidMethods = 0;
    int validMethods[10];
    int methodNum = -1;

    int needed = -1;
    int needsNetwork = 0;

    int rhcdfnd = 0;

    char * devName = NULL;
    static struct networkDeviceConfig netDev;

    char * kbdtype = NULL;

    for (i = 0; i < numMethods; i++, numValidMethods++) {
        installNames[numValidMethods] = installMethods[i].name;
        validMethods[numValidMethods] = i;
    }
    installNames[numValidMethods] = NULL;

    /* have we preselected this to be our install method? */
    if (loaderData->method >= 0) {
        methodNum = loaderData->method;
        /* disable the fast path (#102652) */
        flags |= LOADER_FLAGS_ASKMETHOD;
    }

    /* check to see if we have a CD.  If we have one, then
     * we can fast-path the CD and not make people answer questions in 
     * text mode.  */
    if (!FL_ASKMETHOD(flags) && !FL_KICKSTART(flags)) {
        url = findAnacondaCD(location, !FL_RESCUE(flags));
        /* if we found a CD and we're not in rescue or vnc mode return */
        /* so we can short circuit straight to stage 2 from CD         */
        if (url && (!FL_RESCUE(flags) && !hasGraphicalOverride()))
            return url;
        else {
            rhcdfnd = 1;
            methodNum = 0;
        }
    }

    if (!FL_CMDLINE(flags))
        startNewt();

    step = STEP_LANG;

    while (step != STEP_DONE) {
        switch(step) {
        case STEP_LANG:
            if (loaderData->lang && (loaderData->lang_set == 1)) {
                setLanguage(loaderData->lang, 1);
            } else {
                chooseLanguage(&loaderData->lang);
            }
            step = STEP_KBD;
            dir = 1;
            break;
        case STEP_KBD:
            if (loaderData->kbd && (loaderData->kbd_set == 1)) {
                /* JKFIXME: this is broken -- we should tell of the 
                 * failure; best by pulling code out in kbd.c to use */
                if (isysLoadKeymap(loaderData->kbd)) {
                    logMessage(WARNING, "requested keymap %s is not valid, asking", loaderData->kbd);
                    loaderData->kbd = NULL;
                    loaderData->kbd_set = 0;
                    break;
                }
                rc = LOADER_NOOP;
            } else {
                /* JKFIXME: should handle kbdtype, too probably... but it 
                 * just matters for sparc */
                if (!FL_CMDLINE(flags))
                    rc = chooseKeyboard(loaderData, &kbdtype);
                else
                   rc = LOADER_NOOP;
            }
            if (rc == LOADER_NOOP) {
                if (dir == -1)
                    step = STEP_LANG;
                else
                    step = STEP_METHOD;
                break;
            }

            if (rc == LOADER_BACK) {
                step = STEP_LANG;
                dir = -1;
            } else {
                step = STEP_METHOD;
                dir = 1;
            }

            break;

        case STEP_METHOD:
            /* this is kind of crappy, but we want the first few questions
             * to be asked when using rescue mode even if we're going
             * to short-circuit to the CD.
             *
             * Alternately, if we're in a VNC install based from CD we
             * can skip this step because we already found the CD */
            if (url) {
                if (FL_RESCUE(flags)) {
                    return url;
                } else if (rhcdfnd) {
                    step = STEP_NETWORK;
                    dir = 1;
                    break;
                }
            }	    

            needed = -1;

            if (loaderData->method != -1 && methodNum != -1) {
                /* dont forget the dir variable. */
                if ( dir == 1 ){
                    rc = 1;
                }else{
                    rc = -1;
                }
            } else {
                /* we need to set these each time through so that we get
                 * updated for language changes (#83672) */
                for (i = 0; i < numMethods; i++) {
                    installNames[i] = _(installMethods[i].name);
                }
                installNames[i] = NULL;

                rc = newtWinMenu(FL_RESCUE(flags) ? _("Rescue Method") :
                                 _("Installation Method"),
                                 FL_RESCUE(flags) ?
                                 _("What type of media contains the rescue "
                                   "image?") :
                                 _("What type of media contains the packages to "
                                   "be installed?"),
                                 30, 10, 20, 6, installNames, &methodNum, 
                                 _("OK"), _("Back"), NULL);
            } 

            if (rc && rc != 1) {
                step = STEP_KBD;
                dir = -1;
            } else {
                needed = installMethods[validMethods[methodNum]].type;
                step = STEP_DRIVER;
                dir = 1;
            }
            break;

        case STEP_DRIVER: {
            if (needed == -1 || haveDeviceOfType(needed)) {
                step = STEP_NETWORK;
                dir = 1;
                needed = -1;
                break;
            }

            rc = newtWinTernary(_("No driver found"), _("Select driver"),
                                _("Use a driver disk"), _("Back"),
                                _("Unable to find any devices of the type "
                                  "needed for this installation type.  "
                                  "Would you like to manually select your "
                                  "driver or use a driver disk?"));
            if (rc == 2) {
                step = STEP_DRIVERDISK;
                dir = 1;
                break;
            } else if (rc == 3) {
                step = STEP_METHOD;
                dir = -1;
                break;
            }

            chooseManualDriver(installMethods[validMethods[methodNum]].type,
                               loaderData);
            /* it doesn't really matter what we return here; we just want
             * to reprobe and make sure we have the driver */
            step = STEP_DRIVER;
            break;
        }

        case STEP_DRIVERDISK:

            rc = loadDriverFromMedia(needed, loaderData, 0, 0);
            if (rc == LOADER_BACK) {
                step = STEP_DRIVER;
                dir = -1;
                break;
            }

            /* need to come back to driver so that we can ensure that we found
             * the right kind of driver after loading the driver disk */
            step = STEP_DRIVER;
            break;

        case STEP_NETWORK:
            if ( (installMethods[validMethods[methodNum]].type !=
                  DEVICE_NETWORK) && (!hasGraphicalOverride()) &&
                 !FL_ASKNETWORK(flags)) {
                needsNetwork = 0;
                if (dir == 1) 
                    step = STEP_URL;
                else if (dir == -1)
                    step = STEP_METHOD;
                break;
            }

            needsNetwork = 1;
            if (!haveDeviceOfType(DEVICE_NETWORK)) {
                needed = DEVICE_NETWORK;
                step = STEP_DRIVER;
                break;
            }
            logMessage(INFO, "need to set up networking");

            initLoopback();
            memset(&netDev, 0, sizeof(netDev));
            netDev.isDynamic = 1;

            /* fall through to interface selection */
        case STEP_IFACE:
            logMessage(INFO, "going to pick interface");

            /* skip configureTCPIP() screen for kickstart (#260621) */
            if (loaderData->ksFile)
                flags |= LOADER_FLAGS_IS_KICKSTART;

            if (FL_HAVE_CMSCONF(flags)) {
                loaderData->ipinfo_set = 1;
                loaderData->ipv6info_set = 1;
            } else {
                loaderData->ipinfo_set = 0;
                loaderData->ipv6info_set = 0;
            }

            rc = chooseNetworkInterface(loaderData);
            if ((rc == LOADER_BACK) || (rc == LOADER_ERROR) ||
                ((dir == -1) && (rc == LOADER_NOOP))) {
                step = STEP_METHOD;
                dir = -1;
                break;
            }

            devName = loaderData->netDev;
            strcpy(netDev.dev.device, devName);

            /* continue to ip config */
            step = STEP_IP;
            dir = 1;
            break;
        case STEP_IP:
            if (!needsNetwork || dir == -1) {
                step = STEP_METHOD; /* only hit going back */
                break;
            }

            if ((ret = malloc(48)) == NULL) {
                logMessage(ERROR, "malloc failure for ret in STEP_IP");
                exit(EXIT_FAILURE);
            }

            logMessage(INFO, "going to do getNetConfig");

            /* s390 provides all config info by way of the CMS conf file */
            if (FL_HAVE_CMSCONF(flags)) {
                loaderData->ipinfo_set = 1;
                loaderData->ipv6info_set = 1;
            }

            /* populate netDev based on any kickstart data */
            if (loaderData->ipinfo_set) {
                netDev.preset = 1;
            }
            setupNetworkDeviceConfig(&netDev, loaderData);

            rc = readNetConfig(devName, &netDev, loaderData->netCls, methodNum);
            if (FL_NOIPV4(flags)) {
                loaderData->ipinfo_set = 0;
            } else {
                if (loaderData->ipv4 == NULL) {
                    if (strcmp((char *) &(netDev.dev.ip), "")) {
                        ret = (char *) inet_ntop(AF_INET,
                                                 IP_ADDR(&(netDev.dev.ip)), ret,
                                                 IP_STRLEN(&(netDev.dev.ip)));
                    } else {
                        ret = NULL;
                        netDev.isDynamic = 1;
                    }

                    if (netDev.isDynamic || ret == NULL) {
                        loaderData->ipv4 = strdup("dhcp");
                    } else {
                        loaderData->ipv4 = strdup(ret);
                    }
                }

                loaderData->ipinfo_set = 1;
            }

            if (FL_NOIPV6(flags)) {
                loaderData->ipv6info_set = 0;
            } else {
                if (loaderData->ipv6 == NULL) {
                    if (strcmp((char *) &(netDev.dev.ip), "")) {
                        ret = (char *) inet_ntop(AF_INET6,
                                                 IP_ADDR(&(netDev.dev.ip)), ret,
                                                 IP_STRLEN(&(netDev.dev.ip)));
                    } else {
                        ret = NULL;
                        netDev.isDynamic = 1;
                    }

                    if (netDev.isDynamic || ret == NULL) {
                        loaderData->ipv6 = strdup("dhcpv6");
                    } else {
                        loaderData->ipv6 = strdup(ret);
                    }
                }

                loaderData->ipv6info_set = 1;
            }

            /* set the hostname if we have that */
            if (loaderData->hostname) {
                if (sethostname(loaderData->hostname,
                                strlen(loaderData->hostname))) {
                    logMessage(ERROR, "error setting hostname to %s",
                               loaderData->hostname);
                }
            }

            free(ret);
            ret = NULL;

            if ((rc == LOADER_BACK) || (rc == LOADER_ERROR) ||
                ((dir == -1) && (rc == LOADER_NOOP))) {
                step = STEP_IFACE;
                dir = -1;
                break;
            }

            writeNetInfo("/tmp/netinfo", &netDev);
            step = STEP_URL;
            dir = 1;
            break;
        case STEP_URL:
            logMessage(INFO, "starting to STEP_URL");
            /* if we found a CD already short circuit out */
            /* we get this case when we're doing a VNC install from CD */
            /* and we didnt short circuit earlier because we had to */
            /* prompt for network info for vnc to work */
            if (url && rhcdfnd)
                return url;

            url = installMethods[validMethods[methodNum]].mountImage(
                                      installMethods + validMethods[methodNum],
                                      location, loaderData);
            if (!url) {
                step = STEP_IP ;
                loaderData->ipinfo_set = 0;
                loaderData->ipv6info_set = 0;
                dir = -1;
            } else {
                logMessage(INFO, "got url %s", url);
                step = STEP_DONE;
                dir = 1;
            }
            break;
        default:
            break;
        }
    }

    return url;
}

static int manualDeviceCheck(struct loaderData_s *loaderData) {
    char ** devices;
    int i, j, rc, num = 0;
    unsigned int width = 40;
    char * buf;

    do {
        /* FIXME */
        devices = malloc(1 * sizeof(*devices));
        j = 0;
        devices[j] = NULL;

        if (width > 70)
            width = 70;

        if (j > 0) {
            buf = _("The following devices have been found on your system.");
        } else {
            buf = _("No device drivers have been loaded for your system.  "
                    "Would you like to load any now?");
        }

        rc = newtWinMenu(_("Devices"), buf, width, 10, 20, 
                         (j > 6) ? 6 : j, devices, &num, _("Done"), 
                         _("Add Device"), NULL);

        /* no leaky */
        for (i = 0; i < j; i++)
            free(devices[j]);
        free(devices);

        if (rc != 2)
            break;

        chooseManualDriver(DEVICE_ANY, loaderData);
    } while (1);
    return 0;
}

/* JKFIXME: I don't really like this, but at least it isolates the ifdefs */
/* Either move dirname to %s_old or unlink depending on arch (unlink on all
 * !s390{,x} arches).  symlink to /mnt/runtime/dirname.  dirname *MUST* start
 * with a '/' */
static void migrate_runtime_directory(char * dirname) {
    char * runtimedir;
    int ret;

    ret = asprintf(&runtimedir, "/mnt/runtime%s", dirname);
    if (!access(runtimedir, X_OK)) {
        if (unlink(dirname) == -1) {
            char * olddir;

            ret = asprintf(&olddir, "%s_old", dirname);
            ret = rename(dirname, olddir);
            free(olddir);
        }
        ret = symlink(runtimedir, dirname);
    }
    free(runtimedir);
}


static int hasGraphicalOverride() {
    int i;

    if (getenv("DISPLAY"))
        return 1;

    for (i = 0; extraArgs[i] != NULL; i++) {
        if (!strncasecmp(extraArgs[i], "--vnc", 5))
            return 1;
    }
    return 0;
}

void loaderSegvHandler(int signum) {
    void *array[30];
    size_t i;
    const char const * const errmsgs[] = {
        "loader received SIG",
        "!  Backtrace:\n",
    };

    /* XXX This should really be in a glibc header somewhere... */
    extern const char *const sys_sigabbrev[NSIG];

    signal(signum, SIG_DFL); /* back to default */

    newtFinished();
    i = write(STDERR_FILENO, errmsgs[0], strlen(errmsgs[0]));
    i = write(STDERR_FILENO, sys_sigabbrev[signum],
            strlen(sys_sigabbrev[signum]));
    i = write(STDERR_FILENO, errmsgs[1], strlen(errmsgs[1]));

    i = backtrace (array, 30);
    backtrace_symbols_fd(array, i, STDERR_FILENO);
    exit(1);
}

static int anaconda_trace_init(void) {
#ifdef USE_MTRACE
    setenv("MALLOC_TRACE","/malloc",1);
    mtrace();
#endif
    /* We have to do this before we init bogl(), which doLoaderMain will do
     * when setting fonts for different languages.  It's also best if this
     * is well before we might take a SEGV, so they'll go to tty8 */
    initializeTtys();

#if 0
    int fd = open("/dev/tty8", O_RDWR);
    if (fd != STDERR_FILENO) {
        close(STDERR_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
#endif

    /* set up signal handler */
    signal(SIGSEGV, loaderSegvHandler);
    signal(SIGABRT, loaderSegvHandler);

    return 0;
}

static void add_to_path_env(const char *env, const char *val)
{
    char *oldenv, *newenv;
    int rc;

    oldenv = getenv(env);
    if (oldenv) {
        rc = asprintf(&newenv, "%s:%s", val, oldenv);
        oldenv = strdupa(newenv);
        free(newenv);
        newenv = oldenv;
    } else {
        newenv = strdupa(val);
    }

    setenv(env, newenv, 1);
}

int main(int argc, char ** argv) {
    int rc;

    struct stat sb;
    struct serial_struct si;
    char * arg;
    FILE *f;

    char twelve = 12;

    moduleInfoSet modInfo;

    char *url = NULL;

    char ** argptr, ** tmparg;
    char * anacondaArgs[50];
    int useRHupdates = 0;

    struct loaderData_s loaderData;

    char *path;
    char * cmdLine = NULL;
    char * ksFile = NULL;
    int testing = 0;
    int mediacheck = 0;
    char * virtpcon = NULL;
    poptContext optCon;
    struct poptOption optionTable[] = {
        { "cmdline", '\0', POPT_ARG_STRING, &cmdLine, 0, NULL, NULL },
        { "ksfile", '\0', POPT_ARG_STRING, &ksFile, 0, NULL, NULL },
        { "test", '\0', POPT_ARG_NONE, &testing, 0, NULL, NULL },
        { "mediacheck", '\0', POPT_ARG_NONE, &mediacheck, 0, NULL, NULL},
        { "virtpconsole", '\0', POPT_ARG_STRING, &virtpcon, 0, NULL, NULL },
        { 0, 0, 0, 0, 0, 0, 0 }
    };

    /* Make sure sort order is right. */
    setenv ("LC_COLLATE", "C", 1);	

    /* Very first thing, set up tracebacks and debug features. */
    rc = anaconda_trace_init();

    /* now we parse command line options */
    optCon = poptGetContext(NULL, argc, (const char **) argv, optionTable, 0);

    if ((rc = poptGetNextOpt(optCon)) < -1) {
        fprintf(stderr, "bad option %s: %s\n",
                poptBadOption(optCon, POPT_BADOPTION_NOALIAS), 
                poptStrerror(rc));
        exit(1);
    }

    if ((arg = (char *) poptGetArg(optCon))) {
        fprintf(stderr, "unexpected argument: %s\n", arg);
        exit(1);
    }

    if (!testing && !access("/var/run/loader.run", R_OK)) {
        printf(_("loader has already been run.  Starting shell.\n"));
        execl("/bin/sh", "-/bin/sh", NULL);
        exit(0);
    }

    f = fopen("/var/run/loader.run", "w+");
    fprintf(f, "%d\n", getpid());
    fclose(f);

    /* The fstat checks disallows serial console if we're running through
       a pty. This is handy for Japanese. */
    fstat(0, &sb);
    if (major(sb.st_rdev) != 3 && major(sb.st_rdev) != 136 && 
        (virtpcon == NULL)){
        if ((ioctl (0, TIOCLINUX, &twelve) < 0) && 
            (ioctl(0, TIOCGSERIAL, &si) != -1))
            flags |= LOADER_FLAGS_SERIAL;
    }

    if (testing) flags |= LOADER_FLAGS_TESTING;
    if (mediacheck) flags |= LOADER_FLAGS_MEDIACHECK;
    if (ksFile) flags |= LOADER_FLAGS_KICKSTART;
    if (virtpcon) flags |= LOADER_FLAGS_VIRTPCONSOLE;

    /* uncomment to send mac address in ks=http:/ header by default*/
    flags |= LOADER_FLAGS_KICKSTART_SEND_MAC;

    /* JKFIXME: I do NOT like this... it also looks kind of bogus */
#if defined(__s390__) && !defined(__s390x__)
    flags |= LOADER_FLAGS_NOSHELL | LOADER_FLAGS_NOUSB;
#endif

    openLog(FL_TESTING(flags));
    if (!FL_TESTING(flags))
        openlog("loader", 0, LOG_LOCAL0);

    memset(&loaderData, 0, sizeof(loaderData));
    loaderData.method = -1;
    loaderData.fw_loader_pid = -1;
    loaderData.fw_search_pathz_len = -1;
    loaderData.dhcpTimeout = -1;

    extraArgs[0] = NULL;
    parseCmdLineFlags(&loaderData, cmdLine);

    logMessage(INFO, "anaconda version %s on %s starting", VERSION, getProductArch());

    if ((FL_SERIAL(flags) || FL_VIRTPCONSOLE(flags)) && 
        !hasGraphicalOverride()) {
        logMessage(INFO, "text mode forced due to serial/virtpconsole");
        flags |= LOADER_FLAGS_TEXT;
    }
    set_fw_search_path(&loaderData, "/firmware:/lib/firmware");
    start_fw_loader(&loaderData);

    arg = FL_TESTING(flags) ? "./module-info" : "/lib/modules/module-info";
    modInfo = newModuleInfoSet();
    if (readModuleInfo(arg, modInfo, NULL, 0)) {
        fprintf(stderr, "failed to read %s\n", arg);
        sleep(5);
        stop_fw_loader(&loaderData);
        exit(1);
    }
    initializeConsole();

    checkForRam();

    /* iSeries vio console users will be ssh'ing in to the primary
       partition, so use a terminal type that is appripriate */
    if (isVioConsole())
        setenv("TERM", "vt100", 1);

    mlLoadModuleSet("cramfs:vfat:nfs:loop:floppy:edd:pcspkr:squashfs:ext4dev:ext3:ext2:iscsi_tcp");

    if (!FL_NOIPV6(flags))
        mlLoadModule("ipv6", NULL);

    /* now let's do some initial hardware-type setup */
    dasdSetup();
#if defined(__powerpc__)
    mlLoadModule("spufs", NULL);
#endif

    if (loaderData.lang && (loaderData.lang_set == 1)) {
        setLanguage(loaderData.lang, 1);
    }

    /* FIXME: this is a bit of a hack */
    loaderData.modInfo = modInfo;

    if (FL_MODDISK(flags)) {
        startNewt();

        loadDriverDisks(DEVICE_ANY, &loaderData);
    }

    if (!access("/dd.img", R_OK)) {
        logMessage(INFO, "found /dd.img, loading drivers");
        getDDFromSource(&loaderData, "path:/dd.img");
    }

    /* this allows us to do an early load of modules specified on the
     * command line to allow automating the load order of modules so that
     * eg, certain scsi controllers are definitely first.
     * FIXME: this syntax is likely to change in a future release
     *        but is done as a quick hack for the present.
     */
    mlInitModuleConfig();
    earlyModuleLoad(0);

    busProbe(FL_NOPROBE(flags));

    /* JKFIXME: we'd really like to do this before the busprobe, but then
     * we won't have network devices available (and that's the only thing
     * we support with this right now */
    if (loaderData.ddsrc != NULL) {
        getDDFromSource(&loaderData, loaderData.ddsrc);
    }

    /* JKFIXME: loaderData->ksFile is set to the arg from the command line,
     * and then getKickstartFile() changes it and sets FL_KICKSTART.  
     * kind of weird. */
    if (loaderData.ksFile || ksFile) {
        logMessage(INFO, "getting kickstart file");

        if (!ksFile)
            getKickstartFile(&loaderData);
        if (FL_KICKSTART(flags) && 
            (ksReadCommands((ksFile)?ksFile:loaderData.ksFile)!=LOADER_ERROR)) {
            runKickstart(&loaderData);
        }
    }

    if (FL_TELNETD(flags))
        startTelnetd(&loaderData);

    url = doLoaderMain("/mnt/source", &loaderData, modInfo);

    if (!FL_TESTING(flags)) {
        int ret;

        /* unlink dirs and link to the ones in /mnt/runtime */
        migrate_runtime_directory("/usr");
        migrate_runtime_directory("/lib");
        migrate_runtime_directory("/lib64");
        ret = symlink("/mnt/runtime/etc/selinux", "/etc/selinux");
        copyDirectory("/mnt/runtime/etc","/etc", NULL, copyErrorFn);
        copyDirectory("/mnt/runtime/var","/var", NULL, copyErrorFn);
    }

    /* now load SELinux policy before exec'ing anaconda and the shell
     * (if we're using SELinux) */
    if (FL_SELINUX(flags)) {
        if (mount("/selinux", "/selinux", "selinuxfs", 0, NULL)) {
            logMessage(ERROR, "failed to mount /selinux: %s, disabling SELinux", strerror(errno));
            flags &= ~LOADER_FLAGS_SELINUX;
        } else {
            if (loadpolicy() == 0) {
                setexeccon(ANACONDA_CONTEXT);
            } else {
                logMessage(ERROR, "failed to load policy, disabling SELinux");
                flags &= ~LOADER_FLAGS_SELINUX;
            }
        }
    }

    logMessage(INFO, "getting ready to spawn shell now");

    spawnShell();  /* we can attach gdb now :-) */

    if (FL_NOPROBE(flags) && !loaderData.ksFile) {
        startNewt();
        manualDeviceCheck(&loaderData);
    }

    useRHupdates = 0;
    if (loaderData.updatessrc)
        loadUpdatesFromRemote(loaderData.updatessrc, &loaderData);
    else if (FL_UPDATES(flags))
        loadUpdates(&loaderData);

    /* we only want to use RHupdates on nfs installs.  otherwise, we'll 
     * use files on the first iso image and not be able to umount it */
    if (!strncmp(url, "nfs:", 4) && !FL_STAGE2(flags)) {
        logMessage(INFO, "NFS install method detected, will use RHupdates/");
        useRHupdates = 1;
    }

    /* make sure /tmp/updates exists so that magic in anaconda to */
    /* symlink rhpl/ will work                                    */
    if (access("/tmp/updates", F_OK))
        mkdirChain("/tmp/updates");

    add_fw_search_dir(&loaderData, "/tmp/updates/firmware");
    add_fw_search_dir(&loaderData, "/tmp/product/firmware");

    add_to_path_env("PYTHONPATH", "/tmp/updates");
    add_to_path_env("PYTHONPATH", "/tmp/product");
    add_to_path_env("LD_LIBRARY_PATH", "/tmp/updates");
    add_to_path_env("LD_LIBRARY_PATH", "/tmp/product");
    add_to_path_env("PATH", "/tmp/updates");
    add_to_path_env("PATH", "/tmp/product");

    if (useRHupdates) {
        add_to_path_env("PYTHONPATH", "/mnt/source/RHupdates");
        add_to_path_env("LD_LIBRARY_PATH", "/mnt/source/RHupdates");
        add_to_path_env("PATH", "/mnt/source/RHupdates");
        add_fw_search_dir(&loaderData, "/mnt/source/RHupdates/firmware");
    }

    stop_fw_loader(&loaderData);
    start_fw_loader(&loaderData);

    mlLoadModuleSet("md:raid0:raid1:raid5:raid6:raid456:raid10:linear:fat:msdos:jbd:lock_nolock:gfs2:reiserfs:jfs:xfs:dm-mod:dm-zero:dm-mirror:dm-snapshot:dm-multipath:dm-round-robin:dm-emc:dm-crypt:blkcipher:cbc:aes:sha256");

    if (!access("/mnt/runtime/usr/lib/libunicode-lite.so.1", R_OK))
        setenv("LD_PRELOAD", "/mnt/runtime/usr/lib/libunicode-lite.so.1", 1);
    if (!access("/mnt/runtime/usr/lib64/libunicode-lite.so.1", R_OK))
        setenv("LD_PRELOAD", "/mnt/runtime/usr/lib64/libunicode-lite.so.1", 1);

    argptr = anacondaArgs;

    path = getenv("PATH");
    while (path && path[0]) {
        int ret, n = strcspn(path, ":");
        char c, *binpath;

        c = path[n];
        path[n] = '\0';
        ret = asprintf(&binpath, "%s/anaconda", path);
        path[n] = c;

        if (!access(binpath, X_OK)) {
            *argptr++ = strdupa(binpath);
            free(binpath);
            break;
        }
        free(binpath);
        path += n + 1;
    }

    logMessage(INFO, "Running anaconda script %s", *(argptr-1));

    *argptr++ = "-m";
    if (strncmp(url, "ftp:", 4)) {
        *argptr++ = url;
    } else {
        int fd, ret;

        fd = open("/tmp/method", O_CREAT | O_TRUNC | O_RDWR, 0600);
        ret = write(fd, url, strlen(url));
        ret = write(fd, "\r", 1);
        close(fd);
        *argptr++ = "@/tmp/method";
    }

    /* add extra args - this potentially munges extraArgs */
    tmparg = extraArgs;
    while (*tmparg) {
        char *idx;

        logMessage(DEBUGLVL, "adding extraArg %s", *tmparg);
        idx = strchr(*tmparg, '=');
        if (idx &&  ((idx-*tmparg) < strlen(*tmparg))) {
            *idx = '\0';
            *argptr++ = *tmparg;
            *argptr++ = idx+1;
        } else {
            *argptr++ = *tmparg;
        }

        tmparg++;
    }

    if (FL_NOIPV4(flags))
        *argptr++ = "--noipv4";

    if (FL_NOIPV6(flags))
        *argptr++ = "--noipv6";

    if (FL_RESCUE(flags)) {
        *argptr++ = "--rescue";
        if (FL_SERIAL(flags))
            *argptr++ = "--serial";
    } else {
        if (FL_SERIAL(flags))
            *argptr++ = "--serial";
        if (FL_TEXT(flags))
            *argptr++ = "-T";
        else if (FL_GRAPHICAL(flags))
            *argptr++ = "--graphical";
        if (FL_CMDLINE(flags))
            *argptr++ = "-C";
        if (FL_EXPERT(flags))
            *argptr++ = "--expert";
        if (!FL_SELINUX(flags))
            *argptr++ = "--noselinux";
        else if (FL_SELINUX(flags))
            *argptr++ = "--selinux";

        if (FL_KICKSTART(flags)) {
            *argptr++ = "--kickstart";
            *argptr++ = loaderData.ksFile;
        }

        if (FL_VIRTPCONSOLE(flags)) {
            *argptr++ = "--virtpconsole";
            *argptr++ = virtpcon;
        }

        if (loaderData.updatessrc && FL_UPDATES(flags)) {
            *argptr++ = "--updates";
            *argptr++ = loaderData.updatessrc;
        }

        if (loaderData.dogtailurl) {
            *argptr++ = "--dogtail";
            *argptr++ = loaderData.dogtailurl;
        }

        if ((loaderData.lang) && !FL_NOPASS(flags)) {
            *argptr++ = "--lang";
            *argptr++ = loaderData.lang;
        }

        if ((loaderData.kbd) && !FL_NOPASS(flags)) {
            *argptr++ = "--keymap";
            *argptr++ = loaderData.kbd;
        }

        if (loaderData.logLevel) {
            *argptr++ = "--loglevel";
            *argptr++ = loaderData.logLevel;
        }
    }
    
    *argptr = NULL;
    
    stopNewt();
    closeLog();
    
    if (!FL_TESTING(flags)) {
        int pid, status, rc;
        char *fmt;

        if (FL_RESCUE(flags)) {
            fmt = _("Running anaconda %s, the %s rescue mode - please wait...\n");
        } else {
            fmt = _("Running anaconda %s, the %s system installer - please wait...\n");
        }
        printf(fmt, VERSION, getProductName());

        if (!(pid = fork())) {
            setenv("ANACONDAVERSION", VERSION, 1);
            if (execv(anacondaArgs[0], anacondaArgs) == -1) {
               fprintf(stderr,"exec of anaconda failed: %s\n",strerror(errno));
               exit(1);
            }
        }

        waitpid(pid, &status, 0);

        if (!WIFEXITED(status) || (WIFEXITED(status) && WEXITSTATUS(status))) {
            rc = 1;
        } else {
            rc = 0;
        }

        if ((rc == 0) && (FL_POWEROFF(flags) || FL_HALT(flags))) {
            if (!(pid = fork())) {
                char * cmd = (FL_POWEROFF(flags) ? strdup("/sbin/poweroff") :
                              strdup("/sbin/halt"));
                if (execl(cmd, cmd, NULL) == -1) {
                    fprintf(stderr, "exec of poweroff failed: %s", 
                            strerror(errno));
                    exit(1);
                }
            }
            waitpid(pid, &status, 0);
        }

#if defined(__s390__) || defined(__s390x__)
        /* FIXME: we have to send a signal to linuxrc on s390 so that shutdown
         * can happen.  this is ugly */
        FILE * f;
        f = fopen("/var/run/init.pid", "r");
        if (!f) {
            logMessage(WARNING, "can't find init.pid, guessing that init is pid 1");
            pid = 1;
        } else {
            char * buf = malloc(256);
            char *ret;

            ret = fgets(buf, 256, f);
            pid = atoi(buf);
            free(buf);
            fclose(f);
        }
        kill(pid, SIGUSR2);
#endif
        stop_fw_loader(&loaderData);
        return rc;
    }
#if 0
    else {
	char **args = anacondaArgs;
	printf("would have run ");
	while (*args)
	    printf("%s ", *args++);
	printf("\n");
	printf("LANGKEY=%s\n", getenv("LANGKEY"));
	printf("LANG=%s\n", getenv("LANG"));
    }
#endif
    return 1;
}
