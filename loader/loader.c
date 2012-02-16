/*
 * loader.c
 *
 * This is the installer loader.  Its job is to somehow load the rest
 * of the installer into memory and run it.  This may require setting
 * up some devices and networking, etc. The main point of this code is
 * to stay SMALL! Remember that, live by that, and learn to like it.
 *
 * Copyright (C) 1997-2012  Red Hat, Inc.
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
 *            David Cantrell <dcantrell@redhat.com>
 */

#include <ctype.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <newt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <arpa/inet.h>

#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>

#include <linux/fb.h>
#include <linux/serial.h>
#include <linux/vt.h>

#include <glib.h>

#ifdef USE_MTRACE
#include <mcheck.h>
#endif

#include "serial.h"
#include "copy.h"
#include "getparts.h"
#include "loader.h"
#include "loadermisc.h" /* JKFIXME: functions here should be split out */
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
#include "urls.h"
#include "urlinstall.h"

#include "ibft.h"
#include "net.h"
#include "readvars.h"
#include "unpack.h"
#ifdef USESELINUX
#include <selinux/selinux.h>
#endif
#include "selinux.h"

#include "../pyanaconda/isys/imount.h"
#include "../pyanaconda/isys/isys.h"
#include "../pyanaconda/isys/lang.h"
#include "../pyanaconda/isys/eddsupport.h"
#include "../pyanaconda/isys/log.h"
#include "../pyanaconda/isys/mem.h"

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

static int init_sig = SIGUSR1; /* default to shutdown=halt */

static char *VIRTIO_PORT = "/dev/virtio-ports/org.fedoraproject.anaconda.log.0";

static struct installMethod installMethods[] = {
    { N_("Local CD/DVD"), "METHOD_CDROM", 0, DEVICE_CDROM, promptForCdrom, loadCdromImages},
    { N_("Hard drive"), "METHOD_HD", 0, DEVICE_DISK, promptForHardDrive, loadHdImages },
    { N_("NFS directory"), "METHOD_NFS", 1, DEVICE_NETWORK, promptForNfs, loadNfsImages },
    { "URL", "METHOD_URL", 1, DEVICE_NETWORK, promptForUrl, loadUrlImages},
};
static int numMethods = sizeof(installMethods) / sizeof(struct installMethod);

static int expected_exit = 0;

static GHashTable *cmdline = NULL;

void doExit(int result)
{
    expected_exit = 1;
    exit(result);
}

void doSuspend(void) {
    newtFinished();
    doExit(1);
}

void doShell(void) {
    pid_t child;
    int status;

    newtSuspend();
    child = fork();

    if (child == 0) {
        if (chdir("/root") == 0) {
            if (execl("/bin/bash", "/bin/bash", "-i", NULL) == -1) {
                logMessage(ERROR, "%s (%d): %m", __func__, __LINE__);
                _exit(1);
            }
        } else {
            logMessage(ERROR, "missing /root, cannot run shell");
        }
    } else if (child == -1) {
        logMessage(ERROR, "%s (%d): %m", __func__, __LINE__);
        newtResume();
    } else {
        if (waitpid(child, &status, 0) == -1) {
            logMessage(ERROR, "%s (%d): %m", __func__, __LINE__);
        }

        newtResume();
    }
}

void doGdbserver(struct loaderData_s *loaderData) {
    int child, fd;
    char *pid;
    iface_t iface;

    /* If gdbserver is found, go ahead and run it on the loader process now
     * before anything bad happens.
     */
    if (loaderData->gdbServer && !access("/usr/bin/gdbserver", X_OK)) {
        pid_t loaderPid = getpid();
        iface_init_iface_t(&iface);

#if !defined(__s390__) && !defined(__s390x__)
        if (kickstartNetworkUp(loaderData, &iface)) {
            logMessage(ERROR, "can't run gdbserver due to no network");
            return;
        }
#endif

        checked_asprintf(&pid, "%d", loaderPid);

        if (!(child = fork())) {
            logMessage(INFO, "starting gdbserver: %s %s %s %s",
                       "/usr/bin/gdbserver", "--attach", loaderData->gdbServer,
                       pid);

            fd = open("/dev/null", O_RDONLY);
            close(STDIN_FILENO);
            dup2(fd, STDIN_FILENO);
            close(fd);
            fd = open("/dev/null", O_WRONLY);
            close(STDOUT_FILENO);
            dup2(fd, STDOUT_FILENO);
            close(STDERR_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);

            if (execl("/usr/bin/gdbserver", "/usr/bin/gdbserver", "--attach",
                      loaderData->gdbServer, pid, NULL) == -1)
                logMessage(ERROR, "error running gdbserver: %m");

            _exit(1);
        }
    }
}

void startNewt(void) {
    if (!newtRunning) {
        char *buf;
        char *arch = getProductArch();
        checked_asprintf(&buf, _("Welcome to %s for %s"), getProductName(), arch);

        /*
         * Because currently initrd.img only has got the default English locale
         * support, pretend for newtInit() it is actually the used LANG so Newt
         * knows how to compute character widths etc. 
         */
        char *lang = getenv("LANG");
        if (lang) {
            lang = strdup(lang);
        }
        setenv("LANG", LANG_DEFAULT, 1);
        newtInit();
        unsetenv("LANG");
        /* restore the original LANG value */
        if (lang) {
            setenv("LANG", lang, 1);
            free(lang);
        }

        newtCls();
        newtDrawRootText(0, 0, buf);
        free(buf);
        
        newtPushHelpLine(_("  <Tab>/<Alt-Tab> between elements  | <Space> selects | <F12> next screen "));
        
        newtRunning = 1;
        if (!access("/bin/sh",  X_OK)) 
            newtSetSuspendCallback((void *) doShell, NULL);
    }
}

void stopNewt(void) {
    if (newtRunning) newtFinished();
    newtRunning = 0;
}

static gchar *productName = NULL;
static gchar *productArch = NULL;

/* The product info comes from ./.buildstamp, which is an .ini-style file
 * written out by scripts/mk-images and put into every initrd.  It contains
 * information about the product being booted, which is what differentiates it
 * from .treeinfo.  A valid file looks like this:
 *
 * [Main]
 * BugURL=http://bugzilla.redhat.com
 * IsFinal=true|false
 * Product=<name of the product - Fedora, RHEL, etc.>
 * UUID=<time and date stamp, then a dot, then the architecture>
 * Version=<version of the product - 6.0, 14, etc.>
 *
 * Any other key/value pairs will be silently ignored.
 */
static void initProductInfo(void) {
    GKeyFile *key_file = g_key_file_new();
    GError *loadErr = NULL;

    if (!g_key_file_load_from_file(key_file, "/.buildstamp", G_KEY_FILE_NONE, &loadErr)) {
        logMessage(ERROR, "error reading .buildstamp: %s", loadErr->message);
        g_error_free(loadErr);
        productName = g_strdup("anaconda");
        productArch = g_strdup("unknown architecture");
        return;
    }

    if (g_key_file_has_key(key_file, "Main", "Product", NULL))
        productName = g_key_file_get_string(key_file, "Main", "Product", NULL);
    else
        productName = g_strdup("anaconda");

    if (g_key_file_has_key(key_file, "Main", "UUID", NULL)) {
        gchar **parts = g_strsplit(g_key_file_get_string(key_file, "Main", "UUID", NULL), ".", 0);

        if (parts != NULL && g_strv_length(parts) == 2)
            productArch = g_strdup(parts[1]);
        else
            productArch = g_strdup("unknown architecture");

        g_strfreev(parts);
    } else {
        productArch = g_strdup("unknown architecture");
    }

    return;
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

void initializeConsole() {
    /* enable UTF-8 console */
    setenv("LANG", LANG_DEFAULT, 1);
    printf("\033%%G");
    fflush(stdout);

    isysLoadFont();
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

static void copyWarnFn (char *msg) {
   logMessage(WARNING, msg);
}

static void copyErrorFn (char *msg) {
   newtWinMessage(_("Error"), _("OK"), _(msg));
}

void loadUpdates(struct loaderData_s *loaderData) {
    char *device = NULL, *part = NULL, *buf;
    char **devNames = NULL;
    enum { UPD_DEVICE, UPD_PART, UPD_PROMPT, UPD_LOAD, UPD_DONE } stage = UPD_DEVICE;
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
                devNames = NULL;
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
                devNames = NULL;
                return;
            }

            device = strdup(devNames[num]);
            free(devNames);
            devNames = NULL;
            stage = UPD_PART;
        }

        case UPD_PART: {
            char ** part_list = getPartitionsList(device);
            int nump = 0, num = 0;

            if (part != NULL) {
                free(part);
                part = NULL;
            }

            if ((nump = g_strv_length(part_list)) == 0) {
                if (dir == -1) {
                    stage = UPD_DEVICE;
                } else {
                    checked_asprintf(&part, "/dev/%s", device);
                    stage = UPD_PROMPT;
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
                g_strfreev(part_list);
                stage = UPD_DEVICE;
                dir = -1;
                break;
            }

            part = strdup(part_list[num]);
            stage = UPD_LOAD;
        }

        case UPD_PROMPT:
            checked_asprintf(&buf, _("Insert your updates disk into %s and "
                                     "press \"OK\" to continue."), part);

            rc = newtWinChoice(_("Updates Disk"), _("OK"), _("Back"), buf);
            free(buf);
            buf = NULL;

            if (rc == 2) {
                stage = UPD_PART;
                dir = -1;
                break;
            }

            stage = UPD_LOAD;
            break;

        case UPD_LOAD:
            logMessage(INFO, "UPDATES device is %s", part);

            if (doPwMount(part, "/mnt/install/update-disk", "auto", "ro", NULL)) {
                newtWinMessage(_("Error"), _("OK"),
                               _("Failed to mount updates disk"));
                stage = UPD_PROMPT;
                break;
            } else {
                /* Copy everything to /tmp/updates so we can unmount the disk  */
                winStatus(40, 3, _("Updates"), _("Reading anaconda updates"));
                if (!copyDirectory("/mnt/install/update-disk", "/tmp/updates", copyWarnFn,
                                   copyErrorFn)) {
                    dir = 1;
                    stage = UPD_DONE;
                }

                newtPopWindow();
                umount("/mnt/install/update-disk");
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

/* parse anaconda or pxelinux-style ip= arguments
 * pxelinux format: ip=<client-ip>:<boot-server-ip>:<gw-ip>:<netmask>
 * anaconda format: ip=<client-ip> netmask=<netmask> gateway=<gw-ip>
*/
static void parseCmdLineIp(struct loaderData_s * loaderData, char *argv)
{
    /* Detect pxelinux */
    if (strstr(argv, ":") != NULL) {
        char *start = argv;
        char *end;

        end = strstr(start, ":");
        loaderData->ipv4 = strndup(start, end-start);
        loaderData->ipinfo_set = 1;

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
        loaderData->ipv4 = strdup(argv);
        loaderData->ipinfo_set = 1;
    }

    if (loaderData->ipinfo_set)
        flags |= LOADER_FLAGS_IP_PARAM;
}

#ifdef ENABLE_IPV6
/*
 * parse anaconda ipv6= arguments
 */
static void parseCmdLineIpv6(struct loaderData_s * loaderData, char *argv)
{
    loaderData->ipv6 = strdup(argv);
    loaderData->ipv6info_set = 1;
    flags |= LOADER_FLAGS_IPV6_PARAM;
}
#endif

static long argToLong(char *arg) {
    long retval;

    errno = 0;

    retval = strtol(arg, NULL, 10);
    if ((errno == ERANGE && (retval == LONG_MIN || retval == LONG_MAX)) ||
        (errno != 0 && retval == 0)) {
        logMessage(ERROR, "%s: %d: %m", __func__, __LINE__);
        abort();
    }

    return retval;
}

/* parses /proc/cmdline for any arguments which are important to us.  */
static void parseCmdLineFlags(struct loaderData_s * loaderData) {
    int numExtraArgs = 0;
    GHashTableIter iter;
    gpointer key = NULL, value = NULL;

    /* we want to default to graphical and allow override with 'text' */
    flags |= LOADER_FLAGS_GRAPHICAL;

    if (cmdline == NULL) {
        logMessage(INFO, "kernel command line was empty");
        return;
    }

    logMessage(INFO, "kernel command line:");
    g_hash_table_iter_init(&iter, cmdline);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gchar *k = (gchar *) key;
        gchar *v = (gchar *) value;

        if (v == NULL) {
            logMessage(INFO, "    %s", k);
        } else {
            logMessage(INFO, "    %s=%s", k, v);
        }

        /* handle installer boot arguments */
        if (!strcasecmp(k, "askmethod")) {
            flags |= LOADER_FLAGS_ASKMETHOD;
        } else if (!strcasecmp(k, "asknetwork")) {
            flags |= LOADER_FLAGS_ASKNETWORK;
        } else if (!strcasecmp(k, "noshell")) {
            flags |= LOADER_FLAGS_NOSHELL;
        } else if (!strcasecmp(k, "nokill")) {
            flags |= LOADER_FLAGS_NOKILL;
        } else if (!strcasecmp(k, "mediacheck")) {
            flags |= LOADER_FLAGS_MEDIACHECK;
        } else if (!strcasecmp(k, "allowwireless")) {
            flags |= LOADER_FLAGS_ALLOW_WIRELESS;
        } else if (!strcasecmp(k, "noprobe")) {
            flags |= LOADER_FLAGS_NOPROBE;
        } else if (!strcasecmp(k, "text")) {
            logMessage(INFO, "text mode forced from cmdline");
            flags |= LOADER_FLAGS_TEXT;
            flags &= ~LOADER_FLAGS_GRAPHICAL;
        } else if (!strcasecmp(k, "graphical")) {
            logMessage(INFO, "graphical mode forced from cmdline");
            flags |= LOADER_FLAGS_GRAPHICAL;
        } else if (!strcasecmp(k, "cmdline")) {
            logMessage(INFO, "cmdline mode forced from cmdline");
            flags |= LOADER_FLAGS_CMDLINE;
        } else if (!strcasecmp(k, "updates")) {
            if (v != NULL) {
                loaderData->updatessrc = g_strdup(v);
            } else {
                flags |= LOADER_FLAGS_UPDATES;
            }
        } else if (!strcasecmp(k, "dd") || !strcasecmp(k, "driverdisk")) {
            if (v != NULL) {
                loaderData->ddsrc = g_strdup(v);
            } else {
                flags |= LOADER_FLAGS_MODDISK;
            }
        } else if (!strcasecmp(k, "rescue")) {
            flags |= LOADER_FLAGS_RESCUE;
        } else if (!strcasecmp(k, "nopass")) {
            flags |= LOADER_FLAGS_NOPASS;
        } else if (!strcasecmp(k, "serial")) {
            flags |= LOADER_FLAGS_SERIAL;
        } else if (!strcasecmp(k, "noipv4")) {
            flags |= LOADER_FLAGS_NOIPV4;
#ifdef ENABLE_IPV6
        } else if (!strcasecmp(k, "noipv6")) {
            flags |= LOADER_FLAGS_NOIPV6;
#endif
        } else if (!strcasecmp(k, "kssendmac")) {
            flags |= LOADER_FLAGS_KICKSTART_SEND_MAC;
        } else if (!strcasecmp(k, "kssendsn")) {
            flags |= LOADER_FLAGS_KICKSTART_SEND_SERIAL;
        /* deprecated hardware bits */
        } else if (!strcasecmp(k, "nousb")) {
            mlAddBlacklist("ehci-hcd");
            mlAddBlacklist("ohci-hcd");
            mlAddBlacklist("uhci-hcd");
        } else if (!strcasecmp(k, "nofirewire")) {
            mlAddBlacklist("firewire-ohci");
        } else if (!strcasecmp(k, "ks")) {
            GString *tmp = g_string_new("ks");

            if (v != NULL) {
                g_string_append_printf(tmp, "=%s", v);
            }

            loaderData->ksFile = g_strdup(tmp->str);
            g_string_free(tmp, TRUE);
        } else if (!strcasecmp(k, "selinux")) {
            if (v != NULL && !strcasecmp(v, "0")) {
                flags &= ~LOADER_FLAGS_SELINUX;
            } else {
                flags |= LOADER_FLAGS_SELINUX;
            }
        } else if (!strcasecmp(k, "noeject")) {
            flags |= LOADER_FLAGS_NOEJECT;
#if !defined(__s390__) && !defined(__s390x__)
        } else if (!strcasecmp(k, "sshd")) {
            logMessage(INFO, "early networking required for sshd");
            flags |= LOADER_FLAGS_EARLY_NETWORKING;
#endif
        } else if (!strcasecmp(k, "noverifyssl")) {
            flags |= LOADER_FLAGS_NOVERIFYSSL;
        } else if (v != NULL) {
            /* boot arguments that are of the form name=value */
            /* all arguments in this block require the value  */

            if (!strcasecmp(k, "dlabel")) {
                if (!strcasecmp(v, "on")) {
                    flags |= LOADER_FLAGS_AUTOMODDISK;
                } else if (!strcasecmp(v, "off")) {
                    flags &= ~LOADER_FLAGS_AUTOMODDISK;
                }
            } else if (!strcasecmp(k, "loglevel")) {
                if (!strcasecmp(v, "debug")) {
                    loaderData->logLevel = g_strdup(v);
                    setLogLevel(DEBUGLVL);
                } else if (!strcasecmp(v, "info")) {
                    loaderData->logLevel = g_strdup(v);
                    setLogLevel(INFO);
                } else if (!strcasecmp(v, "warning")) {
                    loaderData->logLevel = g_strdup(v);
                    setLogLevel(WARNING);
                } else if (!strcasecmp(v, "error")) {
                    loaderData->logLevel = g_strdup(v);
                    setLogLevel(ERROR);
                } else if (!strcasecmp(v, "critical")) {
                    loaderData->logLevel = g_strdup(v);
                    setLogLevel(CRITICAL);
                }
            } else if (!strcasecmp(k, "ksdevice")) {
                loaderData->netDev = g_strdup(v);
                loaderData->netDev_set = 1;
            } else if (!strcmp(k, "BOOTIF")) {
                /* trim leading '01-' if it exists */
                if (!strncmp(v, "01-", 3)) {
                    loaderData->bootIf = g_strdup(v + 3);
                } else {
                    loaderData->bootIf = g_strdup(v);
                }

                loaderData->bootIf_set = 1;

                /* scan the BOOTIF value and replace '-' with ':' */
                char *front = loaderData->bootIf;
                if (front) {
                    while (*front != '\0') {
                        if (*front == '-') {
                            *front = ':';
                        }

                        front++;
                    }
                }
            } else if (!strcasecmp(k, "dhcpclass")) {
                loaderData->netCls = g_strdup(v);
                loaderData->netCls_set = 1;
            } else if (!strcasecmp(k, "display")) {
                setenv("DISPLAY", v, 1);
            } else if (!strcasecmp(k, "lang")) {
                loaderData->lang = normalizeLang(v);
                loaderData->lang_set = 1;
            } else if (!strcasecmp(k, "keymap")) {
                loaderData->kbd = g_strdup(v);
                loaderData->kbd_set = 1;
            } else if (!strcasecmp(k, "method")) {
                logMessage(WARNING, "method= is deprecated.  Please use repo= instead.");
                loaderData->instRepo = g_strdup(v);
            } else if (!strcasecmp(k, "stage2")) {
                logMessage(WARNING, "stage2= is deprecated.  Please use repo= instead.");
                flags |= LOADER_FLAGS_ASKMETHOD;
            } else if (!strcasecmp(k, "repo")) {
                loaderData->instRepo = g_strdup(v);

                /* We still have to set the method in case repo= was used so we
                 * know how to get updates.img and product.img, and it only
                 * gets set elsewhere on interactive installs.
                 */
                if (!strncmp(v, "nfs:", 4))
                    loaderData->method = METHOD_NFS;
                else if (!strncmp(v, "hd:", 3))
                    loaderData->method = METHOD_HD;
                else if (!strncmp(v, "cd:", 3))
                    loaderData->method = METHOD_CDROM;
            } else if (!strcasecmp(k, "hostname")) {
                loaderData->hostname = g_strdup(v);
            } else if (!strcasecmp(k, "ip")) {
                parseCmdLineIp(loaderData, v);
#ifdef ENABLE_IPV6
            } else if (!strcasecmp(k, "ipv6")) {
                parseCmdLineIpv6(loaderData, v);
#endif
            } else if (!strcasecmp(k, "netmask")) {
                loaderData->netmask = g_strdup(v);
            } else if (!strcasecmp(k, "gateway")) {
                char *gateway = g_strdup(v);
                if (isValidIPv4Address(gateway)) {
                    loaderData->gateway = gateway;
#ifdef ENABLE_IPV6
                } else if (isValidIPv6Address(gateway)) {
                    loaderData->gateway6 = gateway;
#endif
                } else {
                    logMessage(WARNING,
                               "invalid address in boot option gateway");
                    free(gateway);
                }
            } else if (!strcasecmp(k, "dns")) {
                loaderData->dns = g_strdup(v);
            } else if (!strcasecmp(k, "ethtool")) {
                loaderData->ethtool = g_strdup(v);
            } else if (!strcasecmp(k, "essid")) {
                loaderData->essid = g_strdup(v);
            } else if (!strcasecmp(k, "mtu")) {
                loaderData->mtu = argToLong(v);
            } else if (!strcasecmp(k, "wepkey")) {
                loaderData->wepkey = g_strdup(v);
            } else if (!strcasecmp(k, "wpakey")) {
                loaderData->wpakey = g_strdup(v);
            } else if (!strcasecmp(k, "linksleep")) {
                num_link_checks = argToLong(v);
            } else if (!strcasecmp(k, "nicdelay")) {
                post_link_sleep = argToLong(v);
            } else if (!strcasecmp(k, "dhcptimeout")) {
                loaderData->dhcpTimeout = argToLong(v);
            } else if (!strcasecmp(k, "gdb")) {
                loaderData->gdbServer = g_strdup(v);
            } else if (!strcasecmp(k, "proxy")) {
                splitProxyParam(v, &loaderData->proxyUser,
                                &loaderData->proxyPassword, &loaderData->proxy);
            }
        }

        if (numExtraArgs < (MAX_EXTRA_ARGS - 1)) {
            /* go through and append args we just want to pass on to  */
            /* the anaconda script, but don't want to represent as a  */
            /* LOADER_FLAGS_XXX since loader doesn't care about these */
            /* particular options.                                    */
            /* do vncpassword case first */
            if (!strcasecmp(k, "vncpassword") && v != NULL) {
                writeVNCPasswordFile("/tmp/vncpassword.dat", v);
            } else if (!strcasecmp(k, "resolution") ||
                       !strcasecmp(k, "nomount") ||
                       !strcasecmp(k, "vnc") ||
                       !strcasecmp(k, "vncconnect") ||
                       !strcasecmp(k, "headless") ||
                       !strcasecmp(k, "usefbx") ||
                       !strcasecmp(k, "mpath") ||
                       !strcasecmp(k, "nompath") ||
                       !strcasecmp(k, "dmraid") ||
                       !strcasecmp(k, "nodmraid") ||
                       !strcasecmp(k, "xdriver") ||
                       !strcasecmp(k, "syslog")) {

                /* vnc implies graphical */
                if (!strcasecmp(k, "vnc")) {
                    logMessage(INFO, "vnc forced graphical mode from cmdline");
                    flags |= LOADER_FLAGS_GRAPHICAL;
                }

                /* the following things require networking to be configured
                 * by loader, so an active connection is ready once we get
                 * to anaconda
                 */
#if !defined(__s390__) && !defined(__s390x__)
                if (!strcasecmp(k, "syslog") || !strcasecmp(k, "vnc")) {
                    logMessage(INFO, "early networking required for %s", k);
                    flags |= LOADER_FLAGS_EARLY_NETWORKING;
                }

                if (isKickstartFileRemote(loaderData->ksFile)) {
                    logMessage(INFO, "early networking required for remote kickstart configuration");
                    flags |= LOADER_FLAGS_EARLY_NETWORKING;
                }
#endif

                if (v != NULL) {
                    checked_asprintf(&extraArgs[numExtraArgs], "--%s=%s", k, v)
                } else {
                    checked_asprintf(&extraArgs[numExtraArgs],"--%s", k);
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

    /* NULL terminates the array of extra args */
    extraArgs[numExtraArgs] = NULL;

    return;
}

/* make sure they have enough ram */
static void checkForRam(int install_method) {
    char *reason = _("%s requires %d MB of memory, to install, but you only have %d MB.");
    int needed = MIN_RAM;
    int installed = totalMemory();

    if (totalMemory() < needed) {
        char *buf;
        checked_asprintf(&buf, reason, getProductName(), needed/1024, installed/1024);

        startNewt();
        newtWinMessage(_("Error"), _("OK"), buf);
        free(buf);
        stopNewt();
        doExit(0);
    }
}

static void checkTaintFlag(void) {
    unsigned long l;
    gchar *contents = NULL;
    GError *fileErr = NULL;

    if (!g_file_get_contents("/proc/sys/kernel/tainted", &contents, NULL, &fileErr)) {
        logMessage(ERROR, "error reading /proc/sys/kernel/tainted: %s", fileErr->message);
        g_error_free(fileErr);
        return;
    }

    errno = 0;
    l = strtol(contents, NULL, 10);
    if (errno == EINVAL || errno == ERANGE) {
        logMessage(ERROR, "%s (%d): %m", __func__, __LINE__);
        g_free(contents);
        return;
    }

    if (l & (1 << 28)) {
        startNewt();
        newtWinMessage(_("Unsupported Hardware Detected"), _("OK"),
                       _("This hardware (or a combination thereof) is not "
                         "supported by Red Hat.  For more information on "
                         "supported hardware, please refer to "
                         "http://www.redhat.com/hardware."));
        stopNewt();
    }

    g_free(contents);
}

static int haveDeviceOfType(int type) {
    struct device ** devices;

    devices = getDevices(type);
    if (devices) {
        return 1;
    }
    return 0;
}

/* 
 * Use anything you can find to determine if we are running on a QEMU virtual
 * machine.
 */
static int onQEMU(void)
{
    const gchar *lookfor = "QEMU Virtual CPU";
    gchar *contents = NULL;
    GError *fileErr = NULL;
    int ret = 0;

    if (!g_file_get_contents("/proc/cpuinfo", &contents, NULL, &fileErr)) {
        fprintf(stderr, "Unable to read /proc/cpuinfo.\n");
        sleep(5);
        return 0;
    }
    if (strstr(contents, lookfor)) {
        ret = 1;
    }
    g_free(contents);
    return ret;
}

/*
 * Detects the non-static part of rsyslog configuration.
 *
 * Remote TCP logging is enabled if syslog= is found on the kernel command
 * line.  Remote virtio-serial logging is enabled if the declared virtio port
 * exists.
 */
static int getSyslog(gchar **addr, gchar **virtiolog) {
    gpointer value = NULL;
    int ret = 0;

    if (g_hash_table_lookup_extended(cmdline, "syslog", NULL, &value)) {
        *addr = (gchar *) value;
        /* address can be either a hostname or IPv4 or IPv6, with or without port;
           thus we only allow the following characters in the address: letters and
           digits, dots, colons, slashes, dashes and square brackets */
        if (g_regex_match_simple("^[\\w.:/\\-\\[\\]]*$", *addr, 0, 0)) {
            ++ret;
        } else {
            /* malformed, disable use */
            *addr = NULL;
            printf("The syslog= command line parameter is malformed and will be\n");
            printf("ignored by the installer.\n");
            sleep(5);
        }
    }

    if (onQEMU()) {
        /* look for virtio-serial logging on a QEMU machine. */
        printf("Looking for the virtio ports... ");
        if (system("/sbin/udevadm trigger --action=add --sysname-match='vport*'") ||
            system("/sbin/udevadm settle")) {
            fprintf(stderr, "Error calling udevadm trigger to get virtio ports.\n");
            sleep(5);
        } else {
            printf("done.\n");
        }
        if (!access(VIRTIO_PORT, W_OK)) {
            /* that means we really have virtio-serial logging */
            *virtiolog = VIRTIO_PORT;
            ++ret;
        }
    }

    return ret;
}

/* sets up and restarts syslog */
static void restartSyslog(void) {
    int conf_fd;
    gchar *addr = NULL, *virtiolog = NULL;
    const char *forward_tcp = "*.* @@";
    const char *forward_format_tcp = "\n";
    const char *forward_virtio = "*.* ";
    const char *forward_format_virtio = ";virtio_ForwardFormat\n";

    /* update the config file with command line arguments first */
    if (!getSyslog(&addr, &virtiolog))
        return;

    conf_fd = open("/etc/rsyslog.conf", O_WRONLY|O_APPEND);
    if (conf_fd < 0) {
        printf("error opening /etc/rsyslog.conf: %d\n", errno);
        printf("syslog forwarding will not be enabled\n");
        sleep(5);
    } else {
        if (addr != NULL) {
            write(conf_fd, forward_tcp, strlen(forward_tcp));
            write(conf_fd, addr, strlen(addr));
            write(conf_fd, forward_format_tcp, strlen(forward_format_tcp));
        }
        if (virtiolog != NULL) {
            write(conf_fd, forward_virtio, strlen(forward_virtio));
            write(conf_fd, virtiolog, strlen(virtiolog));
            write(conf_fd, forward_format_virtio, strlen(forward_format_virtio));
        }
        close(conf_fd);
    }

    /* now we really start the daemon. */
    int status = system("/bin/systemctl --quiet restart rsyslog.service");
    if (status < 0 || 
        !WIFEXITED(status) || 
        WEXITSTATUS(status)  != 0) {
        printf("Unable to start syslog daemon.\n");
        doExit(EXIT_FAILURE);
    }
}

static void doLoaderMain(struct loaderData_s *loaderData,
                         moduleInfoSet modInfo) {
    enum { STEP_LANG, STEP_KBD, STEP_METHOD, STEP_DRIVER,
           STEP_DRIVERDISK, STEP_NETWORK, STEP_IFACE,
           STEP_IP, STEP_EXTRAS, STEP_DONE } step;
    char *step_names[] = { "STEP_LANG", "STEP_KBD", "STEP_METHOD", "STEP_DRIVER",
                           "STEP_DRIVERDISK", "STEP_NETWORK", "STEP_IFACE",
                           "STEP_IP", "STEP_EXTRAS", "STEP_DONE" };

    char *ret = NULL, *devName = NULL, *kbdtype = NULL;
    static iface_t iface;
    int i, rc = LOADER_NOOP, dir = 1;
    int needsNetwork = 0, class = -1;
    int skipLangKbd = 0;

    char *installNames[10];
    int numValidMethods = 0;
    int validMethods[10];

    for (i = 0; i < numMethods; i++, numValidMethods++) {
        installNames[numValidMethods] = installMethods[i].name;
        validMethods[numValidMethods] = i;
    }
    installNames[numValidMethods] = NULL;

    i = 0;
    step = STEP_LANG;

    /* Before anything, check if we're installing from CD/DVD.  If so, we can
     * skip right over most of these steps.  Note that askmethod or repo= will
     * override this check.
     */
    if (!FL_ASKMETHOD(flags) && !loaderData->instRepo && findInstallCD(loaderData) == LOADER_OK) {
        logMessage(DEBUGLVL, "Found installation media, so skipping lang and kbd");
        queryCDMediaCheck(loaderData->instRepo);
        skipLangKbd = 1;
        flags |= LOADER_FLAGS_NOPASS;
    }

    while (step != STEP_DONE) {
        logMessage(DEBUGLVL, "in doLoaderMain, step = %s", step_names[step]);

        switch(step) {
            case STEP_LANG: {
                if (loaderData->lang && (loaderData->lang_set == 1))
                    setLanguage(loaderData->lang, 1);
                else if (FL_RESCUE(flags) || !skipLangKbd)
                    chooseLanguage(&loaderData->lang);

                step = STEP_KBD;
                dir = 1;
                break;
            }

            case STEP_KBD: {
                if (loaderData->kbd && (loaderData->kbd_set == 1)) {
                    /* JKFIXME: this is broken -- we should tell of the 
                     * failure; best by pulling code out in kbd.c to use */
                    if (isysLoadKeymap(loaderData->kbd)) {
                        logMessage(WARNING, "requested keymap %s is not valid, asking",
                                   loaderData->kbd);
                        loaderData->kbd = NULL;
                        loaderData->kbd_set = 0;
                        break;
                    }
                    rc = LOADER_NOOP;
                } else if (FL_RESCUE(flags) || !skipLangKbd) {
                    /* JKFIXME: should handle kbdtype, too probably... but it 
                     * just matters for sparc */
                    if (!FL_CMDLINE(flags))
                        rc = chooseKeyboard(loaderData, &kbdtype);
                    else
                       rc = LOADER_NOOP;
                } else {
                    step = STEP_METHOD;
                    dir = 1;
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
            }

            case STEP_METHOD: {
                if (FL_ASKMETHOD(flags)) {
                    if (FL_CMDLINE(flags)) {
                        fprintf(stderr, "No method given for cmdline mode, aborting\n");
                        doExit(EXIT_FAILURE);
                    }
                } else {
                    step = (dir == -1) ? STEP_KBD : STEP_DRIVER;
                    break;
                }

                /* we need to set these each time through so that we get
                 * updated for language changes (#83672) */
                for (i = 0; i < numMethods; i++) {
                    installNames[i] = _(installMethods[i].name);
                }
                installNames[i] = NULL;

                rc = newtWinMenu(_("Installation Method"),
                                 _("What type of media contains the installation tree?"),
                                 30, 10, 20, 6, installNames, &loaderData->method,
                                 _("OK"), _("Back"), NULL);
                if (rc == 2) {
                    flags |= LOADER_FLAGS_ASKMETHOD;
                }

                if (rc && (rc != 1)) {
                    step = STEP_KBD;
                    dir = -1;
                } else {
                    /* Now prompt for the method-specific config info. */
                    rc = installMethods[validMethods[loaderData->method]].prompt(loaderData);

                    /* Just go back to the method selection screen. */
                    if (rc == LOADER_BACK)
                        break;

                    class = installMethods[validMethods[loaderData->method]].type;
                    step = STEP_DRIVER;
                    dir = 1;
                }

                break;
            }

            case STEP_DRIVER: {
                if ((FL_EARLY_NETWORKING(flags) && haveDeviceOfType(DEVICE_NETWORK)) ||
                    (class == -1 || haveDeviceOfType(class))) {
                    step = STEP_NETWORK;
                    dir = 1;
                    class = -1;
                    break;
                }

                if (skipLangKbd) {
                    skipLangKbd = 0;
                    step = STEP_KBD;
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
                    flags |= LOADER_FLAGS_ASKMETHOD;
                    dir = -1;
                    break;
                }

                chooseManualDriver(installMethods[validMethods[loaderData->method]].type,
                                   loaderData);
                /* it doesn't really matter what we return here; we just want
                 * to reprobe and make sure we have the driver */
                step = STEP_DRIVER;
                break;
            }

            case STEP_DRIVERDISK: {
                if (skipLangKbd) {
                    skipLangKbd = 0;
                    step = STEP_KBD;
                    break;
                }

                rc = loadDriverFromMedia(class, loaderData, 0, 0, NULL);
                if (rc == LOADER_BACK) {
                    step = STEP_DRIVER;
                    dir = -1;
                    break;
                }

                /* need to come back to driver so that we can ensure that we found
                 * the right kind of driver after loading the driver disk */
                step = STEP_DRIVER;
                break;
            }

            case STEP_NETWORK: {
                if ((!isURLRemote(loaderData->instRepo) &&
                      !FL_ASKNETWORK(flags) &&
                      !FL_EARLY_NETWORKING(flags) && 
                      !ibft_present()) ||
                     (is_nm_connected())) {
                    needsNetwork = 0;
                    if (dir == 1) 
                        step = STEP_EXTRAS;
                    else if (dir == -1)
                        step = STEP_METHOD;
                    break;
                }

                needsNetwork = 1;
                if (!haveDeviceOfType(DEVICE_NETWORK)) {
                    class = DEVICE_NETWORK;
                    step = STEP_DRIVER;
                    break;
                }
                logMessage(INFO, "need to set up networking");

                iface_init_iface_t(&iface);

                /* fall through to interface selection */
            }

            case STEP_IFACE: {
                /* skip configureTCPIP() screen for kickstart (#260621) */
                if (loaderData->ksFile)
                    flags |= LOADER_FLAGS_IS_KICKSTART;

#if defined(__s390__) || defined(__s390x__)
                {
                    loaderData->ipinfo_set = 1;
#ifdef ENABLE_IPV6
                    loaderData->ipv6info_set = 1;
#endif
                    step = STEP_EXTRAS;
                    dir = 1;
                    break;
                }
#endif

                rc = chooseNetworkInterface(loaderData);
                if (rc == LOADER_BACK || rc == LOADER_ERROR || (dir == -1 && rc == LOADER_NOOP)) {
                    step = STEP_METHOD;
                    dir = -1;
                    /* Clear current device selection so we prompt on re-entry to STEP_IFACE */
                    loaderData->netDev_set = 0;
                    break;
                }

                devName = loaderData->netDev;
                strcpy(iface.device, devName);

                /* continue to ip config */
                step = STEP_IP;
                dir = 1;
                break;
            }

            case STEP_IP: {
                if (!needsNetwork || dir == -1) {
                    step = STEP_METHOD; /* only hit going back */
                    break;
                }

                if ((ret = malloc(INET6_ADDRSTRLEN+1)) == NULL) {
                    logMessage(ERROR, "malloc failure for ret in STEP_IP");
                    doExit(EXIT_FAILURE);
                }

                /* populate netDev based on any kickstart data */
                logMessage(DEBUGLVL, "in doLoaderMain, calling setupIfaceStruct()");
                setupIfaceStruct(&iface, loaderData);
                logMessage(DEBUGLVL, "in doLoaderMain, calling readNetConfig()");
                rc = readNetConfig(devName, &iface, loaderData->netCls, loaderData->method);

                free(ret);
                ret = NULL;

                /* Go to STEP_IFACE */
                if (rc == LOADER_BACK || (dir == -1 && rc == LOADER_NOOP)) {
                    needsNetwork = 1;
                    step = STEP_IFACE;
                    dir = -1;

                   /* Clear out netDev_set so we prompt the user in
                    * STEP_IFACE */
                   loaderData->netDev_set = 0;
                   /* Clear ipinfo_set so we prompt the user in when they
                    * return to STEP_IP */
                   loaderData->ipinfo_set = 0;
#ifdef ENABLE_IPV6
                   loaderData->ipv6info_set = 0;
#endif
                   logMessage(DEBUGLVL, "in STEP_IP, going to STEP_IFACE (LOADER_BACK)");

                    break;
                }
                /* Retry STEP_IP */
                if (rc == LOADER_ERROR) {
                    needsNetwork = 1;

                   /* Don't retry the same failed settings.  Clear ipinfo-set
                    * so the user can enter new data */
                   loaderData->ipinfo_set = 0;
#ifdef ENABLE_IPV6
                   loaderData->ipv6info_set = 0;
#endif
                   logMessage(DEBUGLVL, "in STEP_IP, retry (LOADER_ERROR)");

                    break;
                }

                writeEnabledNetInfo(&iface);
                /* Prevent asking about interface for updates.img */
                loaderData->netDev_set = 1;
                step = STEP_EXTRAS;
                dir = 1;
                break;
            }

            case STEP_EXTRAS: {
                logMessage(DEBUGLVL, "in STEP_EXTRAS, method = %s",
                           installMethods[validMethods[loaderData->method]].desc);

                installMethods[validMethods[loaderData->method]].findExtras(loaderData);
                step = STEP_DONE;
                break;
            }

            case STEP_DONE:
                break;
        }
    }
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
        "Loader exited unexpectedly!  Backtrace:\n",
    };

    /* XXX This should really be in a glibc header somewhere... */
    extern const char *const sys_sigabbrev[NSIG];

    signal(signum, SIG_DFL); /* back to default */

    newtFinished();
    if (signum == 0) {
        i = write(STDERR_FILENO, errmsgs[2], strlen(errmsgs[2]));
    } else {
        i = write(STDERR_FILENO, errmsgs[0], strlen(errmsgs[0]));
        i = write(STDERR_FILENO, sys_sigabbrev[signum],
                strlen(sys_sigabbrev[signum]));
        i = write(STDERR_FILENO, errmsgs[1], strlen(errmsgs[1]));
    }

    i = backtrace (array, 30);
    if (signum == SIGSEGV)
        backtrace_symbols_fd(array+2, i-2, STDERR_FILENO);
    else
        backtrace_symbols_fd(array+1, i-1, STDERR_FILENO);
    _exit(1);
}

void loaderExitHandler(void)
{
    if (expected_exit)
        return;

    loaderSegvHandler(0);    
}

static void setupBacktrace(void)
{
    void *array;

    signal(SIGSEGV, loaderSegvHandler);
    signal(SIGABRT, loaderSegvHandler);
    atexit(loaderExitHandler);

    /* Turns out, there's an initializer at the top of backtrace() that
     * (on some arches) calls dlopen(). dlopen(), unsurprisingly, calls
     * malloc(). So, call backtrace() early in signal handler setup so
     * we can later safely call it from the signal handler itself. */
    backtrace(&array, 1);
}

void loaderUsrXHandler(int signum) {
    logMessage(INFO, "Remembering signal %d\n", signum);
    init_sig = signum;
}

void restart_anaconda() {
    if (access("/tmp/restart_anaconda", R_OK)) {
        printf("Starting Anaconda version %s.\n", VERSION);
        return;
    }

    puts("Restarting Anaconda.");
    unlink("/tmp/restart_anaconda");

    if (!access("/var/run/loader.run", R_OK))
        unlink("/var/run/loader.run");

    int updates_fd = open("/tmp/updates", O_RDONLY);
    if (recursiveRemove(updates_fd))
        fprintf(stderr, "Error removing /tmp/updates. Updates won't be re-downloaded.");
}

static int anaconda_trace_init(int isDevelMode) {
    /* if anaconda dies suddenly we are doomed, so at least make a coredump */
    struct rlimit corelimit = { RLIM_INFINITY,  RLIM_INFINITY};
    if (setrlimit(RLIMIT_CORE, &corelimit)) {
        perror("setrlimit failed - no coredumps will be available");
    }

#ifdef USE_MTRACE
    setenv("MALLOC_TRACE","/malloc",1);
    mtrace();
#endif
    /* We have to do this before we init bogl(), which doLoaderMain will do
     * when setting fonts for different languages.  It's also best if this
     * is well before we might take a SEGV, so they'll go to tty8 */
    initializeTtys();

    /* set up signal handler unless we want it to crash in devel mode */
    if(!isDevelMode)
        setupBacktrace();

    return 0;
}

static char *setupMallocPerturb(char *value)
{
    FILE *f;
    unsigned char x;
    size_t rc;
    char *ret = NULL;

    f = fopen("/dev/urandom", "r");
    if (!f)
        return NULL;

    rc = fread(&x, 1, 1, f);
    fclose(f);
    if (rc < 1)
        return NULL;

    rc = asprintf(&ret, "MALLOC_PERTURB_=%hhu", x);
    if (rc < 0)
        return NULL;
    return ret;
}

/* these functions return a newly allocated string that never gets freed;
 * their lifetime is essentially that of main(), and we'd have to track which
 * are allocated and which aren't, which is pretty pointless... */
typedef char *(*setupEnvCallback)(char *entry);

static void setupEnv(void)
{
    struct {
        char *name;
        setupEnvCallback cb;
    } setupEnvCallbacks[] = {
        { "MALLOC_PERTURB_", setupMallocPerturb },
        { NULL, NULL }
    };
    int x;

    /* neither array is very big, so this algorithm isn't so bad.  If env[]
     * gets bigger for some reason, we should probably just alphebatize both
     * (manually) and then only initialize y one time.
     */
    for (x = 0; setupEnvCallbacks[x].name != NULL; x++) {
        char *value = getenv(setupEnvCallbacks[x].name);

        if (value != NULL) {
            char *new = setupEnvCallbacks[x].cb(value);
            if (new)
                setenv(setupEnvCallbacks[x].name, new, 1);
        }
    }
}

static void add_to_path_env(const char *env, const char *val)
{
    char *oldenv, *newenv;

    oldenv = getenv(env);
    if (oldenv) {
        checked_asprintf(&newenv, "%s:%s", val, oldenv);

        oldenv = strdupa(newenv);
        free(newenv);
        newenv = oldenv;
    } else {
        newenv = strdupa(val);
    }

    setenv(env, newenv, 1);
}

static void loadScsiDhModules(void)
{
    struct utsname utsname;
    char *modules = NULL;
    char *tmp = NULL;
    struct dirent *ent = NULL;

    uname(&utsname);
    checked_asprintf(&tmp,
        "/lib/modules/%s/kernel/drivers/scsi/device_handler", utsname.release);

    DIR *dir = opendir(tmp);
    free(tmp);
    if (!dir)
        return;

    int fd = dirfd(dir);
    while ((ent = readdir(dir)) != NULL) {
        struct stat sb;

        if (fstatat(fd, ent->d_name, &sb, 0) < 0)
            continue;

        size_t len = strlen(ent->d_name) - 3;
        if (strcmp(ent->d_name+len, ".ko"))
            continue;

        if (S_ISREG(sb.st_mode)) {
            char modname[len+1];
            strncpy(modname, ent->d_name, len);
	    modname[len] = '\0';

            if (modules && modules[0]) {
                checked_asprintf(&tmp, "%s:%s", modules, modname);
            } else {
                checked_asprintf(&tmp, "%s", modname);
            }

            free(modules);
            modules = tmp;
        }
    }
    closedir(dir);

    mlLoadModuleSet(modules);
    free(modules);
}

int main(int argc, char ** argv) {
    int rc;
    int isDevelMode = 0;

    char * arg;
    FILE *f;

    moduleInfoSet modInfo;
#if !defined(__s390__) && !defined(__s390x__)
    iface_t iface;
#endif

    char ** argptr, ** tmparg;
    char * anacondaArgs[50];
    char * outputKSFile = NULL;

    struct loaderData_s loaderData;
    struct termios orig_cmode;
    int orig_flags;
    char *virtpcon = NULL;

    char *path, *fmt;
    GSList *dd, *dditer;
    GTree *moduleState;

    cmdline = readvars_parse_file("/proc/cmdline");

    restartSyslog();
    /* disable console logging so we don't clog the ttys */
    klogctl(8, NULL, 1);

    /* If the anaconda process is now being restarted, we need to do some
     * environment cleanup first.
     */
    restart_anaconda();

    /* check for development mode early */
    if (cmdline && g_hash_table_lookup_extended(cmdline, "devel", NULL, NULL)) {
        printf("Enabling development mode - cores will be dumped\n");
        isDevelMode = 1;
    }

    signal(SIGUSR1, loaderUsrXHandler);
    signal(SIGUSR2, loaderUsrXHandler);

    /* Make sure sort order is right. */
    setenv ("LC_COLLATE", "C", 1);

    /* Very first thing, set up tracebacks and debug features. */
    rc = anaconda_trace_init(isDevelMode);
    setupEnv();

    /* uncomment to send mac address in ks=http:/ header by default*/
    flags |= LOADER_FLAGS_KICKSTART_SEND_MAC;

    /* JKFIXME: I do NOT like this... it also looks kind of bogus */
#if defined(__s390__) || defined(__s390x__)
    flags |= LOADER_FLAGS_NOSHELL;
#endif

    openLog();
    
    /* XXX if RHEL, enable the AUTODD feature by default,
     * but we should come with more general way how to control this */
    if (!strncmp(getProductName(), "Red Hat", 7)) {
        flags |= LOADER_FLAGS_AUTOMODDISK;
    }

    memset(&loaderData, 0, sizeof(loaderData));
    loaderData.method = METHOD_URL;
    loaderData.fw_loader_pid = -1;
    loaderData.fw_search_pathz_len = -1;
    loaderData.dhcpTimeout = -1;

    extraArgs[0] = NULL;
    parseCmdLineFlags(&loaderData);

    /* Set this now so it's valid for all udev trigger calls. */
    system("udevadm control --env=ANACONDA=1");

    if (!access("/var/run/loader.run", R_OK)) {
        printf(_("loader has already been run.  Starting shell.\n"));
        execl("/bin/sh", "-/bin/sh", NULL);
        doExit(0);
    }

    f = fopen("/var/run/loader.run", "w+");
    fprintf(f, "%d\n", getpid());
    fclose(f);

    logMessage(INFO, "anaconda version %s on %s starting", VERSION, getProductArch());

    if (init_serial(&orig_cmode, &orig_flags, cmdline)) {
        /* init_serial replaces fd(0,1,2) with one of three things:
         * /dev/ttyS0, /dev/console, or a fancy virtual console. */
        virtpcon = ttyname(0);
        if (virtpcon != NULL) {
            if (!strcmp(virtpcon, "/dev/ttyS0") || !strcmp(virtpcon, "/dev/console"))
                flags |= LOADER_FLAGS_SERIAL;
            else
                flags |= LOADER_FLAGS_VIRTPCONSOLE;
        }
    }

    if ((FL_SERIAL(flags) || FL_VIRTPCONSOLE(flags)) && 
        !hasGraphicalOverride()) {
        logMessage(INFO, "text mode forced due to serial/virtpconsole");
        flags |= LOADER_FLAGS_TEXT;
    }
#if !defined(__s390__) && !defined(__s390x__)
    if (hasGraphicalOverride())
        flags |= LOADER_FLAGS_EARLY_NETWORKING;
#endif
    set_fw_search_path(&loaderData, "/firmware:/lib/firmware");
    start_fw_loader(&loaderData);

    arg = "/lib/modules/module-info";
    modInfo = newModuleInfoSet();
    if (readModuleInfo(arg, modInfo, NULL, 0)) {
        fprintf(stderr, "failed to read %s\n", arg);
        sleep(5);
        stop_fw_loader(&loaderData);
        doExit(1);
    }
    initializeConsole();

    checkForRam(-1);

    /* iSeries vio console users will be ssh'ing in to the primary
       partition, so use a terminal type that is appripriate */
    if (isVioConsole())
        setenv("TERM", "vt100", 1);

    mlLoadModuleSet("cramfs:squashfs:iscsi_tcp");

    loadScsiDhModules();

#if !defined(__s390__) && !defined(__s390x__)
    mlLoadModuleSet("floppy:edd:pcspkr:iscsi_ibft");
#endif

#ifdef ENABLE_IPV6
    if (!FL_NOIPV6(flags))
        mlLoadModule("ipv6", NULL);
#endif

    /* now let's do some initial hardware-type setup */
#if defined(__powerpc__)
    mlLoadModule("spufs", NULL);
#endif

    if (loaderData.lang && loaderData.lang_set == 1) {
        setLanguage(loaderData.lang, 1);
    } else {
        setLanguage(LANG_DEFAULT, 1);
    }

    /* FIXME: this is a bit of a hack */
    loaderData.modInfo = modInfo;

    /* If there is /.rundepmod file present, rerun depmod */
    if (!access("/.rundepmod", R_OK)){
        if (system("depmod -a")) {
            /* this is not really fatal error, it might still work, log it */
            logMessage(ERROR, "Error running depmod -a for initrd overlay");
        }
    }

    /* this allows us to do an early load of modules specified on the
     * command line to allow automating the load order of modules so that
     * eg, certain scsi controllers are definitely first.
     * FIXME: this syntax is likely to change in a future release
     *        but is done as a quick hack for the present.
     */
    if (!mlInitModuleConfig()) {
        logMessage(ERROR, "unable to initialize kernel module loading");
        abort();
    }

    earlyModuleLoad(0);

    /* Save list of preloaded modules so we can restore the state */
    moduleState = mlSaveModuleState();

    if (FL_AUTOMODDISK(flags)) {
        /* Load all autodetected DDs */
        logMessage(INFO, "Trying to detect vendor driver discs");
        dd = findDriverDiskByLabel();
        dditer = dd;

        if (dd && !loaderData.ksFile) {
            startNewt();
        }


        while(dditer) {
            /* If in interactive mode, ask for confirmation before loading the DD */
            if (!loaderData.ksFile) {
                char * buf;

                checked_asprintf(&buf,
                                 _("Driver disc was detected in %s. "
                                   "Do you want to use it?."), dditer->data);

                rc = newtWinChoice(_("Driver disc detected"), _("Use it"), _("Skip it"),
                                   buf);
                free(buf);
                if (rc == 2) {
                    logMessage(INFO, "Skipping driver disk %s.", (char*)(dditer->data));

                    /* clean the device record */
                    free((char*)(dditer->data));
                    dditer->data = NULL;

                    /* next DD */
                    dditer = g_slist_next(dditer);
                    continue;
                }
            }

            /* load the DD */
            if (loadDriverDiskFromPartition(&loaderData, (char*)(dditer->data))) {
                logMessage(ERROR, "Automatic driver disk loader failed for %s.", (char*)(dditer->data));
            }
            else {
                logMessage(INFO, "Automatic driver disk loader succeeded for %s.", (char*)(dditer->data));

                /* Unload all devices and load them again to use the updated modules */
                mlRestoreModuleState(moduleState);
                busProbe(0);
            }
            
            /* clean the device record */
            free((char*)(dditer->data));
            dditer->data = NULL;

            /* next DD */
            dditer = g_slist_next(dditer);
        }

        if (dd && !loaderData.ksFile) {
            stopNewt();
        }

        g_slist_free(dd);
    }

    if (FL_MODDISK(flags)) {
        startNewt();
        loadDriverDisks(DEVICE_ANY, &loaderData, moduleState);
    }

    if (!access("/dd.img", R_OK)) {
        logMessage(INFO, "found /dd.img, loading drivers");
        getDDFromSource(&loaderData, "path:/dd.img", moduleState);
    }
    
    /* Reset depmod & modprobe to normal mode and get the rest of drivers*/
    mlFreeModuleState(moduleState);
    busProbe(FL_NOPROBE(flags));

    /* Disable all network interfaces in NetworkManager by default */
#if !defined(__s390__) && !defined(__s390x__)
    {
        int i;

        if ((i = writeDisabledNetInfo()) != 0) {
            logMessage(ERROR, "writeDisabledNetInfo failure: %d", i);
        }
    }
#endif

#if defined(__s390__) || defined(__s390x__)
    /* Start NetworkManager until we have systemd init on s390 too */
    if (iface_start_NetworkManager())
        logMessage(INFO, "failed to start NetworkManager");
#else
    /* Restart NetworkManager now so that it uses our inital ifcfg config */
    logMessage(INFO, "restarting NetworkManager");
    if (iface_restart_NetworkManager())
        logMessage(ERROR, "failed to restart NetworkManager");
#endif

    if (!FL_CMDLINE(flags))
        startNewt();

    /* can't run gdbserver until after network modules are loaded */
    doGdbserver(&loaderData);

    /* JKFIXME: we'd really like to do this before the busprobe, but then
     * we won't have network devices available (and that's the only thing
     * we support with this right now */
    if (loaderData.ddsrc != NULL) {
        getDDFromSource(&loaderData, loaderData.ddsrc, NULL);
    }

    /* JKFIXME: loaderData->ksFile is set to the arg from the command line,
     * and then getKickstartFile() changes it and sets FL_KICKSTART.  
     * kind of weird. */
    if (loaderData.ksFile) {
        logMessage(INFO, "getting kickstart file");

        getKickstartFile(&loaderData);
        if (FL_KICKSTART(flags))
            outputKSFile = runKickstart(&loaderData, loaderData.ksFile);
    }

#if !defined(__s390__) && !defined(__s390x__)
    if (FL_EARLY_NETWORKING(flags)) {
        kickstartNetworkUp(&loaderData, &iface);
    }
#endif

    doLoaderMain(&loaderData, modInfo);
#ifdef USESELINUX
    /* now load SELinux policy before exec'ing anaconda and the shell
     * (if we're using SELinux) */
    if (FL_SELINUX(flags)) {
        if (loadpolicy() == 0) {
            setexeccon(ANACONDA_CONTEXT);
        } else {
            logMessage(ERROR, "failed to load policy, disabling SELinux");
            flags &= ~LOADER_FLAGS_SELINUX;
        }
    }
#endif
    if (FL_NOPROBE(flags) && !loaderData.ksFile) {
        startNewt();
        manualDeviceCheck(&loaderData);
    }

    checkTaintFlag();

    if (loaderData.updatessrc)
        loadUpdatesFromRemote(loaderData.updatessrc, &loaderData);
    else if (FL_UPDATES(flags))
        loadUpdates(&loaderData);

    /* make sure /tmp/updates exists so that magic in anaconda to */
    /* symlink rhpl/ will work                                    */
    if (unpack_mkpath("/tmp/updates") == ARCHIVE_OK) {
        add_fw_search_dir(&loaderData, "/tmp/updates/firmware");
        add_to_path_env("PYTHONPATH", "/tmp/updates");
        add_to_path_env("LD_LIBRARY_PATH", "/tmp/updates");
        add_to_path_env("PATH", "/tmp/updates");
    }

    add_fw_search_dir(&loaderData, "/tmp/product/firmware");
    add_to_path_env("PYTHONPATH", "/tmp/product");
    add_to_path_env("LD_LIBRARY_PATH", "/tmp/product");
    add_to_path_env("PATH", "/tmp/product");

    stop_fw_loader(&loaderData);
    start_fw_loader(&loaderData);

    mlLoadModuleSet("raid0:raid1:raid5:raid6:raid456:raid10:linear:dm-mod:dm-zero:dm-mirror:dm-snapshot:dm-multipath:dm-round-robin:dm-crypt:cbc:sha256:lrw:xts");

    argptr = anacondaArgs;

    path = getenv("PATH");
    while (path && path[0]) {
        int n = strcspn(path, ":");
        char c, *binpath;

        c = path[n];
        path[n] = '\0';
        checked_asprintf(&binpath, "%s/anaconda", path);
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

    if (FL_AUTOMODDISK(flags))
        *argptr++ = "--dlabel";

    if (FL_NOIPV4(flags))
        *argptr++ = "--noipv4";

#ifdef ENABLE_IPV6
    if (FL_NOIPV6(flags))
        *argptr++ = "--noipv6";
#endif

#if defined(__s390__) || defined(__s390x__)
    *argptr++ = "--headless";
#endif

    /* Pass the local, preprocessed kickstart file to anaconda.  There's no
     * need for another fetch and another call to preprocessKickstart.
     */
    if (FL_KICKSTART(flags) && outputKSFile) {
        *argptr++ = "--kickstart";
        *argptr++ = outputKSFile;
    }

    if (FL_SERIAL(flags))
        *argptr++ = "--serial";

    if (FL_NOEJECT(flags))
        *argptr++ = "--noeject";

    if (FL_RESCUE(flags)) {
        *argptr++ = "--rescue";
    } else {
        if (FL_TEXT(flags))
            *argptr++ = "-T";
        else if (FL_GRAPHICAL(flags))
            *argptr++ = "--graphical";
        if (FL_CMDLINE(flags))
            *argptr++ = "-C";
        if (!FL_SELINUX(flags))
            *argptr++ = "--noselinux";
        else if (FL_SELINUX(flags))
            *argptr++ = "--selinux";

        if (FL_VIRTPCONSOLE(flags)) {
            *argptr++ = "--virtpconsole";
            *argptr++ = virtpcon;
        }

        if (loaderData.updatessrc && FL_UPDATES(flags)) {
            *argptr++ = "--updates";
            *argptr++ = loaderData.updatessrc;
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

        if (loaderData.instRepo) {
           *argptr++ = "--repo";
            if (strncmp(loaderData.instRepo, "ftp:", 4)) {
                *argptr++ = loaderData.instRepo;
            } else {
                int fd;

                fd = open("/tmp/ftp-repo", O_CREAT | O_TRUNC | O_RDWR, 0600);
                write(fd, loaderData.instRepo, strlen(loaderData.instRepo));
                write(fd, "\r", 1);
                close(fd);
                *argptr++ = "@/tmp/ftp-repo";
            }
        }

        if (loaderData.instRepo_noverifyssl || FL_NOVERIFYSSL(flags)) {
            *argptr++ = "--noverifyssl";
        }

        if (loaderData.proxy && strcmp("", loaderData.proxy)) {
            *argptr++ = "--proxy";

            *argptr++ = strdup(loaderData.proxy);

            if (loaderData.proxyUser && strcmp(loaderData.proxyUser, "")) {
                int fd;

                fd = open("/tmp/proxy", O_CREAT|O_TRUNC|O_RDWR, 0600);
                write(fd, loaderData.proxyUser, strlen(loaderData.proxyUser));
                write(fd, "\r\n", 2);

                if (loaderData.proxyPassword && strcmp(loaderData.proxyPassword, "")) {
                    write(fd, loaderData.proxyPassword, strlen(loaderData.proxyPassword));
                    write(fd, "\r\n", 2);
                }

                close(fd);

                *argptr++ = "--proxyAuth";
                *argptr++ = "/tmp/proxy";
            }
        }
    }

    *argptr = NULL;

    stopNewt();
    closeLog();

    if (FL_RESCUE(flags)) {
        fmt = _("Running anaconda %s, the %s rescue mode - please wait.\n");
    } else {
        fmt = _("Running anaconda %s, the %s system installer - please wait.\n");
    }
    printf(fmt, VERSION, getProductName());

    /* Just replace the current process with anaconda. */
    if (execv(anacondaArgs[0], anacondaArgs) == -1){ 
        fprintf(stderr, "exec of anaconda failed: %m\n");
        doExit(1);
    }

    doExit(0);
}

/* vim:set sw=4 sts=4 et: */
