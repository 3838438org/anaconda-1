/*
 * hardware.c - various hardware probing functionality
 *
 * Erik Troan <ewt@redhat.com>
 * Matt Wilson <msw@redhat.com>
 * Michael Fulbright <msf@redhat.com>
 * Jeremy Katz <katzj@redhat.com>
 *
 * Copyright 1997 - 2003 Red Hat, Inc.
 *
 * This software may be freely redistributed under the terms of the GNU
 * General Public License.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <fcntl.h>
#include <kudzu/kudzu.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include "loader.h"
#include "hardware.h"
#include "pcmcia.h"
#include "log.h"

/* JKFIXME: this is the same hack as in loader.c for second stage modules */
extern struct moduleBallLocation * secondStageModuleLocation;

/* returns whether or not we can probe devices automatically or have to 
 * ask for them manually. */
int canProbeDevices(void) {
#if defined(__s390__) || defined(__s390x__)
    return 1;
#endif

    if ((access("/proc/bus/pci/devices", R_OK) &&
         access("/proc/openprom", R_OK) &&
         access("/proc/iSeries", R_OK)))
        return 1;

    return 0;    
}

static int detectHardware(moduleInfoSet modInfo, 
                          char *** modules, int flags) {
    struct device ** devices, ** device;
    char ** modList;
    int numMods;
    char *driver;
    
    logMessage("probing buses");
    
    devices = probeDevices(CLASS_UNSPEC,
                           BUS_PCI | BUS_SBUS | 
                           ((has_pcmcia() >= 0) ? BUS_PCMCIA : 0),
                           PROBE_ALL);

    logMessage("finished bus probing");
    
    if (devices == NULL) {
        *modules = NULL;
        return LOADER_OK;
    }
    
    numMods = 0;
    for (device = devices; *device; device++) numMods++;
    
    if (!numMods) {
        *modules = NULL;
        return LOADER_OK;
    }
    
    modList = malloc(sizeof(*modList) * (numMods + 1));
    numMods = 0;
    
    for (device = devices; *device; device++) {
        driver = (*device)->driver;
        /* this is kind of icky and verbose.  there are better and more 
         * general ways to do it but this is simple and obvious */
        if (FL_NOPCMCIA(flags) && ((*device)->class == CLASS_SOCKET)) {
            logMessage("ignoring pcmcia device %s (%s)", (*device)->desc,
                       (*device)->driver);
        } else if (FL_NOIEEE1394(flags) && ((*device)->class == CLASS_FIREWIRE)) {
            logMessage("ignoring firewire device %s (%s)", (*device)->desc,
                       (*device)->driver);
        } else if (FL_NOUSB(flags) && ((*device)->class == CLASS_USB)) {
            logMessage("ignoring usb device %s (%s)", (*device)->desc,
                       (*device)->driver);
        } else if (strcmp (driver, "ignore") && strcmp (driver, "unknown")
            && strcmp (driver, "disabled")) {
            modList[numMods++] = strdup(driver);
        }
        
        freeDevice (*device);
    }
    
    modList[numMods] = NULL;
    *modules = modList;
    
    free(devices);
    
    return LOADER_OK;
}

int agpgartInitialize(moduleList modLoaded, moduleDeps modDeps,
                      moduleInfoSet modInfo, int flags) {
    struct device ** devices, *p;
    int i;

    if (FL_TESTING(flags)) return 0;

    logMessage("looking for video cards requiring agpgart module");
    
    devices = probeDevices(CLASS_VIDEO, BUS_UNSPEC, PROBE_ALL);
    
    if (!devices) {
        logMessage("no video cards found");
        return 0;
    }

    /* loop thru cards, see if we need agpgart */
    for (i=0; devices[i]; i++) {
        p = devices[i];
        logMessage("found video card controller %s", p->driver);
        
        /* HACK - need to have list of cards which match!! */
        /* JKFIXME: verify this is really still needed */
        if (!strcmp(p->driver, "Card:Intel 810") ||
            !strcmp(p->driver, "Card:Intel 815")) {
            logMessage("found %s card requiring agpgart, loading module",
                       p->driver+5);
            
            if (mlLoadModuleSetLocation("agpgart", modLoaded, modDeps, 
					modInfo, flags, 
					secondStageModuleLocation)) {
                logMessage("failed to insert agpgart module");
                return 1;
            } else {
                /* only load it once! */
                return 0;
            }
        }
    }
    
    return 0;
}

int scsiTapeInitialize(moduleList modLoaded, moduleDeps modDeps,
                      moduleInfoSet modInfo, int flags) {
    struct device ** devices;

    if (FL_TESTING(flags)) return 0;

    logMessage("looking for scsi tape devices");
    
    devices = probeDevices(CLASS_TAPE, BUS_SCSI, PROBE_ALL);
    
    if (!devices) {
        logMessage("no scsi tape devices found");
        return 0;
    }

    logMessage("scsi tape device(s) found, loading st.o");

    if (mlLoadModuleSetLocation("st", modLoaded, modDeps, 
				modInfo, flags, 
				secondStageModuleLocation)) {
	logMessage("failed to insert st module");
	return 1;
    }
    
    return 0;
}


/* This loads the necessary parallel port drivers for printers so that
   kudzu can autodetect and setup printers in post install*/
void initializeParallelPort(moduleList modLoaded, moduleDeps modDeps,
                            moduleInfoSet modInfo, int flags) {
    /* JKFIXME: this could be useful on other arches too... */
#if !defined (__i386__)
    return;
#endif
    if (FL_NOPARPORT(flags)) return;
    
    logMessage("loading parallel port drivers...");
    if (mlLoadModuleSetLocation("parport_pc", modLoaded, modDeps, 
				modInfo, flags,
				secondStageModuleLocation)) {
        logMessage("failed to load parport_pc module");
        return;
    }
}

void updateKnownDevices(struct knownDevices * kd) {
    kdFindIdeList(kd, 0);
    kdFindScsiList(kd, 0);
    kdFindDasdList(kd, 0);
    kdFindNetList(kd, 0);
}

int busProbe(moduleInfoSet modInfo, moduleList modLoaded, moduleDeps modDeps,
             int justProbe, struct knownDevices * kd, int flags) {
    int i;
    char ** modList;
    char modules[1024];
    
    /* we always want to try to find out about pcmcia controllers even
     * if using noprobe */
    initializePcmciaController(modLoaded, modDeps, modInfo, flags);

    if (FL_NOPROBE(flags)) return 0;
    
    if (canProbeDevices()) {
        /* autodetect whatever we can */
        if (detectHardware(modInfo, &modList, flags)) {
            logMessage("failed to scan pci bus!");
            return 0;
        } else if (modList && justProbe) {
            for (i = 0; modList[i]; i++)
                printf("%s\n", modList[i]);
        } else if (modList) {
            *modules = '\0';
            
            for (i = 0; modList[i]; i++) {
                if (i) strcat(modules, ":");
                strcat(modules, modList[i]);
            }
            
            mlLoadModuleSet(modules, modLoaded, modDeps, modInfo, flags);

            startPcmciaDevices(modLoaded, flags);

            updateKnownDevices(kd);
        } else 
            logMessage("found nothing");
    }
    
    return 0;
}


void scsiSetup(moduleList modLoaded, moduleDeps modDeps,
               moduleInfoSet modInfo, int flags,
               struct knownDevices * kd) {
    mlLoadModuleSet("sd_mod:sr_mod", modLoaded, modDeps, modInfo, flags);
}

void ideSetup(moduleList modLoaded, moduleDeps modDeps,
              moduleInfoSet modInfo, int flags,
              struct knownDevices * kd) {
    mlLoadModuleSet("ide-cd", modLoaded, modDeps, modInfo, flags);
}


/* check if the system has been booted with dasd parameters */
/* These parameters define the order in which the DASDs */
/* are visible to Linux. Otherwise load dasd modules probeonly, */
/* then parse proc to find active DASDs */
/* Reload dasd_mod with correct range of DASD ports */
void dasdSetup(moduleList modLoaded, moduleDeps modDeps,
               moduleInfoSet modInfo, int flags,
               struct knownDevices * kd) {
#if !defined(__s390__) && !defined(__s390x__)
    return;
#else
    char **dasd_parms;
    char *line, *ports = NULL;
    char *parms = NULL, *parms_end;
    FILE *fd;

    dasd_parms = malloc(sizeof(*dasd_parms) * 2);
    dasd_parms[0] = NULL;
    dasd_parms[1] = NULL;

    fd = fopen ("/proc/cmdline", "r");
    if(fd) {
        line = (char *)malloc(sizeof(char) * 200);
        while (fgets (line, 199, fd) != NULL) {
            if((parms = strstr(line, " dasd=")) ||
               (parms = strstr(line, " DASD="))) {
                parms++;
                strncpy(parms, "dasd", 4);
                parms_end = parms;
                while(*parms_end && !(isspace(*parms_end))) parms_end++;
                *parms_end = '\0';
                break;
            }
        }
        fclose(fd);
        free(line);
    }
    if(!parms || (strlen(parms) == 5)) {
        parms = NULL;
    } else {
        dasd_parms[0] = strdup(parms);
        mlLoadModule("dasd_mod", modLoaded, modDeps, modInfo,
                     dasd_parms, flags);

        mlLoadModuleSet("dasd_diag_mod:dasd_fba_mod:dasd_eckd_mod", 
                        modLoaded, modDeps, modInfo, flags);
        return;
    }
    if(!parms) {
        mlLoadModuleSet("dasd_mod:dasd_diag_mod:dasd_fba_mod:dasd_eckd_mod",
                        modLoaded, modDeps, modInfo, flags);
        if((ports = getDasdPorts())) {
            parms = (char *)malloc(strlen("dasd=") + strlen(ports) + 1);
            strcpy(parms,"dasd=");
            strcat(parms, ports);
            dasd_parms[0] = parms;
            simpleRemoveLoadedModule("dasd_eckd_mod", modLoaded, flags);
            simpleRemoveLoadedModule("dasd_fba_mod", modLoaded, flags);
            simpleRemoveLoadedModule("dasd_diag_mod", modLoaded, flags);
            simpleRemoveLoadedModule("dasd_mod", modLoaded, flags);
            reloadUnloadedModule("dasd_mod", modLoaded, dasd_parms, flags);
            reloadUnloadedModule("dasd_eckd_mod", modLoaded, NULL, flags);
            free(dasd_parms);
            free(ports);
        }
    }
#endif
}

