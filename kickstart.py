#
# kickstart.py: kickstart install support
#
# Copyright 1999-2006 Red Hat, Inc.
#
# This software may be freely redistributed under the terms of the GNU
# general public license.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#

import iutil
import isys
import os
from installclass import BaseInstallClass
from partitioning import *
from autopart import *
from fsset import *
from flags import flags
from constants import *
import sys
import raid
import string
import partRequests
import urlgrabber.grabber as grabber
import lvm
import warnings
from pykickstart.constants import *
from pykickstart.parser import *
from pykickstart.data import *
from rhpl.translate import _

import logging
log = logging.getLogger("anaconda")

class AnacondaKSScript(Script):
    def run(self, chroot, serial, intf = None):
        import tempfile
        import os.path

        if self.inChroot:
            scriptRoot = chroot
        else:
            scriptRoot = "/"

        (fd, path) = tempfile.mkstemp("", "ks-script-", scriptRoot + "/tmp")

        os.write(fd, self.script)
        os.close(fd)
        os.chmod(path, 0700)

        if self.logfile is not None:
            messages = self.logfile
        elif serial:
            messages = "/tmp/ks-script.log"
        else:
            messages = "/dev/tty3"

        rc = iutil.execWithRedirect(self.interp, [self.interp,
                                    "/tmp/%s" % os.path.basename(path)],
                                    stdout = messages, stderr = messages,
                                    root = scriptRoot)

        # Always log an error.  Only fail if we have a handle on the
        # windowing system and the kickstart file included --erroronfail.
        if rc != 0:
            log.error("Error code %s encountered running a kickstart %%pre/%%post script", rc)

            if self.errorOnFail:
                if intf != None:
                    intf.messageWindow(_("Scriptlet Failure"),
                                       _("There was an error running the "
                                         "scriptlet.  You may examine the "
                                         "output in %s.  This is a fatal error "
                                         "and your install will be aborted.\n\n"
                                         "Press the OK button to reboot your "
                                         "system.") % (messages,))
                sys.exit(0)

        os.unlink(path)

        if serial or self.logfile is not None:
            os.chmod("%s/%s" % (scriptRoot, messages), 0600)

class AnacondaKSHandlers(KickstartHandlers):
    def __init__ (self, ksdata):
        KickstartHandlers.__init__(self, ksdata)
        self.permanentSkipSteps = []
        self.skipSteps = []
        self.showSteps = []
        self.ksRaidMapping = {}
        self.ksUsedMembers = []
        self.ksPVMapping = {}
        self.ksVGMapping = {}
        # XXX hack to give us a starting point for RAID, LVM, etc unique IDs.
        self.ksID = 100000

        self.lineno = 0
        self.currentCmd = ""

    def doAuthconfig(self, id, args):
        KickstartHandlers.doAuthconfig(self, args)
        id.auth = self.ksdata.authconfig

    def doAutoPart(self, id, args):
        KickstartHandlers.doAutoPart(self, args)

        # sets up default autopartitioning.  use clearpart separately
        # if you want it
        id.instClass.setDefaultPartitioning(id, doClear = 0)

        id.partitions.isKickstart = 1
        self.skipSteps.extend(["partition", "zfcpconfig", "parttype"])

    def doAutoStep(self, id, args):
        KickstartHandlers.doAutoStep(self, args)
        flags.autostep = 1
        flags.autoscreenshot = self.ksdata.autostep["autoscreenshot"]

    def doBootloader (self, id, args):
        KickstartHandlers.doBootloader(self, args)
        dict = self.ksdata.bootloader

        if dict["location"] == "none":
            location = None
        elif dict["location"] == "partition":
            location = "boot"
        else:
            location = dict["location"]

        if dict["upgrade"] and not id.getUpgrade():
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="Selected upgrade mode for bootloader but not doing an upgrade")

        if dict["upgrade"]:
            id.bootloader.kickstart = 1
            id.bootloader.doUpgradeOnly = 1

        if location is None:
            self.permanentSkipSteps.extend(["bootloadersetup", "instbootloader"])
        else:
            self.showSteps.append("bootloadersetup")
            id.instClass.setBootloader(id, location, dict["forceLBA"],
                                       dict["password"], dict["md5pass"],
                                       dict["appendLine"], dict["driveorder"])

        self.permanentSkipSteps.extend(["upgbootloader", "bootloader",
                                        "bootloaderadvanced"])

    def doClearPart(self, id, args):
        KickstartHandlers.doClearPart(self, args)
        dict = self.ksdata.clearpart
        id.instClass.setClearParts(id, dict["type"], drives=dict["drives"],
                                   initAll=dict["initAll"])

    def doDevice(self, id, args):
        KickstartHandlers.doDevice(self, args)

    def doDeviceProbe(self, id, args):
        KickstartHandlers.doDeviceProbe(self, args)

    def doDisplayMode(self, id, args):
        KickstartHandlers.doDisplayMode(self, args)

    def doDriverDisk(self, id, args):
        KickstartHandlers.doDriverDisk(self, args)

    def doFirewall(self, id, args):
        KickstartHandlers.doFirewall(self, args)
        dict = self.ksdata.firewall
	id.instClass.setFirewall(id, dict["enabled"], dict["trusts"],
                                 dict["ports"])

    def doFirstboot(self, id, args):
        KickstartHandlers.doFirstboot(self, args)
        id.firstboot = self.ksdata.firstboot

    def doIgnoreDisk(self, id, args):
	KickstartHandlers.doIgnoreDisk(self, args)
        id.instClass.setIgnoredDisks(id, self.ksdata.ignoredisk)

    def doInteractive(self, id, args):
        KickstartHandlers.doInteractive(self, args)

    def doKeyboard(self, id, args):
        KickstartHandlers.doKeyboard(self, args)
        id.instClass.setKeyboard(id, self.ksdata.keyboard)
        id.keyboard.beenset = 1
	self.skipSteps.append("keyboard")

    def doLang(self, id, args):
        KickstartHandlers.doLang(self, args)
        id.instClass.setLanguage(id, self.ksdata.lang)
	self.skipSteps.append("language")

    def doLangSupport(self, id, args):
        KickstartHandlers.doLangSupport(self, args)

    def doLogicalVolume(self, id, args):
        KickstartHandlers.doLogicalVolume(self, args)
        lvd = self.ksdata.lvList[-1]

        if lvd.mountpoint == "swap":
            filesystem = fileSystemTypeGet("swap")
            lvd.mountpoint = ""

            if lvd.recommended:
                (lvd.size, lvd.maxSizeMB) = iutil.swapSuggestion()
                lvd.grow = True
        else:
            if lvd.fstype != "":
                filesystem = fileSystemTypeGet(lvd.fstype)
            else:
                filesystem = fileSystemTypeGetDefault()

	# sanity check mountpoint
	if lvd.mountpoint != "" and lvd.mountpoint[0] != '/':
	    raise KickstartValueError, formatErrorMsg(self.lineno, msg="The mount point \"%s\" is not valid." % (lvd.mountpoint,))

        if lvd.percent == 0:
            if lvd.size == 0 and not lvd.preexist:
                raise KickstartValueError, formatErrorMsg(self.lineno,
                msg="Size required")
        elif lvd.percent <= 0 or lvd.percent > 100:
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="Percentage must be between 0 and 100")

        try:
            vgid = self.ksVGMapping[lvd.vgname]
        except KeyError:
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="No volume group exists with the name '%s'.  Specify volume groups before logical volumes." % lvd.vgname)

	for areq in id.partitions.autoPartitionRequests:
	    if areq.type == REQUEST_LV:
		if areq.volumeGroup == vgid and areq.logicalVolumeName == lvd.name:
		    raise KickstartValueError, formatErrorMsg(self.lineno, msg="Logical volume name already used in volume group %s" % lvd.vgname)

        if not self.ksVGMapping.has_key(lvd.vgname):
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="Logical volume specifies a non-existent volume group" % lvd.name)

        request = partRequests.LogicalVolumeRequestSpec(filesystem,
                                      format = lvd.format,
                                      mountpoint = lvd.mountpoint,
                                      size = lvd.size,
                                      percent = lvd.percent,
                                      volgroup = vgid,
                                      lvname = lvd.name,
				      grow = lvd.grow,
				      maxSizeMB = lvd.maxSizeMB,
                                      preexist = lvd.preexist,
                                      bytesPerInode = lvd.bytesPerInode)

	if lvd.fsopts != "":
            request.fsopts = lvd.fsopts

        id.instClass.addPartRequest(id.partitions, request)
        self.skipSteps.extend(["partition", "zfcpconfig", "parttype"])

    def doMediaCheck(self, id, args):
        KickstartHandlers.doMediaCheck(self, args)

    def doMethod(self, id, args):
	KickstartHandlers.doMethod(self, args)

    def doMonitor(self, id, args):
        KickstartHandlers.doMonitor(self, args)
        dict = self.ksdata.monitor
        self.skipSteps.extend(["monitor", "checkmonitorok"])
        id.instClass.setMonitor(id, dict["hsync"], dict["vsync"],
                                dict["monitor"])

    def doMouse(self, id, args):
        KickstartHandlers.doMouse(self, args)

    def doNetwork(self, id, args):
        KickstartHandlers.doNetwork(self, args)
        nd = self.ksdata.network[-1]

        try:
            id.instClass.setNetwork(id, nd.bootProto, nd.ip, nd.netmask,
                                    nd.ethtool, nd.device, nd.onboot,
                                    nd.dhcpclass, nd.essid, nd.wepkey)
        except KeyError:
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="The provided network interface %s does not exist" % nd.device)

        if nd.hostname != "":
            id.instClass.setHostname(id, nd.hostname, override=1)

        if nd.nameserver != "":
            id.instClass.setNameserver(id, nd.nameserver)

        if nd.gateway != "":
            id.instClass.setGateway(id, nd.gateway)

        self.skipSteps.append("network")

    def doDmRaid(self, id, args):
        KickstartHandlers.doDmRaid(self, args)

        from partedUtils import DiskSet
        ds = DiskSet()
        ds.startDmRaid()

        raid = self.ksdata.dmraids[-1]
        log.debug("Searching for dmraid '%s'" % (raid.name,))
        for rs in DiskSet.dmList or []:
            it = True
            for dev in raid.devices:
                dev = dev.split('/')[-1]
                log.debug("dmraid '%s' has members %s" % (rs.name, list(rs.members)))
                if not dev in rs.members:
                    log.debug("dmraid '%s' does not have device %s, skipping" \
                        % (rs.name, dev))
                    it = False
            if it:
                log.debug("found dmraid '%s', changing name to %s" \
                    % (rs.name, raid.name))
                # why doesn't rs.name go through the setter here?
                newname = raid.name
                ds.renameDmRaid(rs, newname)
                return
        ds.startDmRaid()

    def doPartition(self, id, args):
        KickstartHandlers.doPartition(self, args)
        pd = self.ksdata.partitions[-1]
        uniqueID = None

        if pd.onbiosdisk != "":
            pd.disk = isys.doGetBiosDisk(pd.onbiosdisk)

            if pd.disk != "":
                raise KickstartValueError, formatErrorMsg(self.lineno, msg="Specified BIOS disk %s cannot be determined" % pd.disk)

        if pd.mountpoint == "swap":
            filesystem = fileSystemTypeGet('swap')
            pd.mountpoint = ""
            if pd.recommended:
                (pd.size, pd.maxSizeMB) = iutil.swapSuggestion()
                pd.grow = True
        # if people want to specify no mountpoint for some reason, let them
        # this is really needed for pSeries boot partitions :(
        elif pd.mountpoint == "None":
            pd.mountpoint = ""
            if pd.fstype:
                filesystem = fileSystemTypeGet(pd.fstype)
            else:
                filesystem = fileSystemTypeGetDefault()
        elif pd.mountpoint == 'appleboot':
            filesystem = fileSystemTypeGet("Apple Bootstrap")
            pd.mountpoint = ""
        elif pd.mountpoint == 'prepboot':
            filesystem = fileSystemTypeGet("PPC PReP Boot")
            pd.mountpoint = ""
        elif pd.mountpoint.startswith("raid."):
            filesystem = fileSystemTypeGet("software RAID")
            
            if self.ksRaidMapping.has_key(pd.mountpoint):
                raise KickstartValueError, formatErrorMsg(self.lineno, msg="Defined RAID partition multiple times")
            
            # get a sort of hackish id
            uniqueID = self.ksID
            self.ksRaidMapping[pd.mountpoint] = uniqueID
            self.ksID = self.ksID + 1
            pd.mountpoint = ""
        elif pd.mountpoint.startswith("pv."):
            filesystem = fileSystemTypeGet("physical volume (LVM)")

            if self.ksPVMapping.has_key(pd.mountpoint):
                raise KickstartValueError, formatErrorMsg(self.lineno, msg="Defined PV partition multiple times")

            # get a sort of hackish id
            uniqueID = self.ksID
            self.ksPVMapping[pd.mountpoint] = uniqueID
            self.ksID = self.ksID + 1
            pd.mountpoint = ""
        # XXX should we let people not do this for some reason?
        elif pd.mountpoint == "/boot/efi":
            filesystem = fileSystemTypeGet("vfat")
        else:
            if pd.fstype != "":
                filesystem = fileSystemTypeGet(pd.fstype)
            else:
                filesystem = fileSystemTypeGetDefault()

        if pd.size is None and (pd.start == 0 and pd.end == 0) and pd.onPart == "":
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="Partition requires a size specification")
        if pd.start != 0 and pd.disk == "":
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="Partition command with start cylinder requires a drive specification")
        hds = isys.hardDriveDict()
        if not hds.has_key(pd.disk) and hds.has_key('mapper/'+pd.disk):
            pd.disk = 'mapper/' + pd.disk
        if pd.disk != "" and pd.disk not in hds.keys():
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="Specified disk in partition command which does not exist")

        request = partRequests.PartitionSpec(filesystem,
                                             mountpoint = pd.mountpoint,
                                             format = pd.format,
                                             fslabel = pd.label,
                                             bytesPerInode = pd.bytesPerInode)
        
        if pd.size is not None:
            request.size = pd.size
        if pd.start != 0:
            request.start = pd.start
        if pd.end != 0:
            request.end = pd.end
        if pd.grow:
            request.grow = pd.grow
        if pd.maxSizeMB != 0:
            request.maxSizeMB = pd.maxSizeMB
        if pd.disk != "":
            request.drive = [ pd.disk ]
        if pd.primOnly:
            request.primary = pd.primOnly
        if uniqueID:
            request.uniqueID = uniqueID
        if pd.onPart != "":
            request.device = pd.onPart
            for areq in id.partitions.autoPartitionRequests:
                if areq.device is not None and areq.device == pd.onPart:
		    raise KickstartValueError, formatErrorMsg(self.lineno, "Partition already used")

        if pd.fsopts != "":
            request.fsopts = pd.fsopts

        id.instClass.addPartRequest(id.partitions, request)
        id.partitions.isKickstart = 1
        self.skipSteps.extend(["partition", "zfcpconfig", "parttype"])

    def doReboot(self, id, args):
        KickstartHandlers.doReboot(self, args)
        self.skipSteps.append("complete")

    def doRaid(self, id, args):
        KickstartHandlers.doRaid(self, args)
        rd = self.ksdata.raidList[-1]

	uniqueID = None

        if rd.mountpoint == "swap":
            filesystem = fileSystemTypeGet('swap')
            rd.mountpoint = ""
        elif rd.mountpoint.startswith("pv."):
            filesystem = fileSystemTypeGet("physical volume (LVM)")

            if self.ksPVMapping.has_key(rd.mountpoint):
                raise KickstartValueError, formatErrorMsg(self.lineno, msg="Defined PV partition multiple times")

            # get a sort of hackish id
            uniqueID = self.ksID
            self.ksPVMapping[rd.mountpoint] = uniqueID
            self.ksID = self.ksID + 1
            rd.mountpoint = ""
        else:
            if rd.fstype != "":
                filesystem = fileSystemTypeGet(rd.fstype)
            else:
                filesystem = fileSystemTypeGetDefault()

	# sanity check mountpoint
	if rd.mountpoint != "" and rd.mountpoint[0] != '/':
	    raise KickstartValueError, formatErrorMsg(self.lineno, msg="The mount point is not valid.")

        raidmems = []

        # get the unique ids of each of the raid members
        for member in rd.members:
            if member not in self.ksRaidMapping.keys():
                raise KickstartValueError, formatErrorMsg(self.lineno, msg="Tried to use undefined partition %s in RAID specification" % member)
	    if member in self.ksUsedMembers:
                raise KickstartValueError, formatErrorMsg(self.lineno, msg="Tried to use RAID member %s in two or more RAID specifications" % member)
		
            raidmems.append(self.ksRaidMapping[member])
	    self.ksUsedMembers.append(member)

        if rd.level == "" and not rd.preexist:
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="RAID Partition defined without RAID level")
        if len(raidmems) == 0 and not rd.preexist:
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="RAID Partition defined without any RAID members")

        request = partRequests.RaidRequestSpec(filesystem,
                                               mountpoint = rd.mountpoint,
                                               raidmembers = raidmems,
                                               raidlevel = rd.level,
                                               raidspares = rd.spares,
                                               format = rd.format,
                                               raidminor = rd.device,
                                               preexist = rd.preexist)

        if uniqueID is not None:
            request.uniqueID = uniqueID
        if rd.preexist and rd.device != "":
            request.device = "md%s" % rd.device
        if rd.fsopts != "":
            request.fsopts = rd.fsopts

        id.instClass.addPartRequest(id.partitions, request)
        self.skipSteps.extend(["partition", "zfcpconfig", "parttype"])

    def doRootPw(self, id, args):
        KickstartHandlers.doRootPw(self, args)
        dict = self.ksdata.rootpw
        
	id.instClass.setRootPassword(id, dict["password"], dict["isCrypted"])
	self.skipSteps.append("accounts")

    def doSELinux(self, id, args):
        KickstartHandlers.doSELinux(self, args)
        id.instClass.setSELinux(id, self.ksdata.selinux)

    def doSkipX(self, id, args):
        KickstartHandlers.doSkipX(self, args)
        self.skipSteps.extend(["checkmonitorok", "setsanex", "videocard",
                               "monitor", "xcustom", "handleX11pkgs",
                               "writexconfig"])

        if id.xsetup is not None:
            id.xsetup.skipx = 1

    def doTimezone(self, id, args):
        KickstartHandlers.doTimezone(self, args)
        dict = self.ksdata.timezone

	id.instClass.setTimezoneInfo(id, dict["timezone"], dict["isUtc"])
	self.skipSteps.append("timezone")

    def doUpgrade(self, id, args):
        KickstartHandlers.doUpgrade(self, args)
        id.setUpgrade(True)

    def doVnc(self, id, args):
        KickstartHandlers.doVnc(self, args)

    def doVolumeGroup(self, id, args):
        KickstartHandlers.doVolumeGroup(self, args)
        vgd = self.ksdata.vgList[-1]

        pvs = []

        # get the unique ids of each of the physical volumes
        for pv in vgd.physvols:
            if pv not in self.ksPVMapping.keys():
                raise KickstartValueError, formatErrorMsg(self.lineno, msg="Tried to use undefined partition %s in Volume Group specification" % pv)
            pvs.append(self.ksPVMapping[pv])

        if len(pvs) == 0 and not vgd.preexist:
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="Volume group defined without any physical volumes.  Either specify physical volumes or use --useexisting.")

        if vgd.pesize not in lvm.getPossiblePhysicalExtents(floor=1024):
            raise KickstartValueError, formatErrorMsg(self.lineno, msg="Volume group specified invalid pesize")

        # get a sort of hackish id
        uniqueID = self.ksID
        self.ksVGMapping[vgd.vgname] = uniqueID
        self.ksID = self.ksID + 1
            
        request = partRequests.VolumeGroupRequestSpec(vgname = vgd.vgname,
                                                      physvols = pvs,
                                                      preexist = vgd.preexist,
                                                      format = vgd.format,
                                                      pesize = vgd.pesize)
        request.uniqueID = uniqueID
        id.instClass.addPartRequest(id.partitions, request)

    def doXConfig(self, id, args):
        KickstartHandlers.doXConfig(self, args)
        dict = self.ksdata.xconfig

        id.instClass.configureX(id, dict["driver"], dict["videoRam"],
                                dict["resolution"], dict["depth"],
                                dict["startX"])
        id.instClass.setDesktop(id, dict["defaultdesktop"])
        self.skipSteps.extend(["videocard", "monitor", "xcustom",
                               "handleX11pkgs", "checkmonitorok", "setsanex"])

    def doZeroMbr(self, id, args):
        KickstartHandlers.doZeroMbr(self, args)
        id.instClass.setZeroMbr(id, 1)

    def doZFCP(self, id, args):
        KickstartHandlers.doZFCP(self, args)
        dict = self.ksdata.zfcp

        dict["devnum"] = id.zfcp.sanitizeDeviceInput(dict["devnum"])
        dict["fcplun"] = id.zfcp.sanitizeHexInput(dict["fcplun"])
        dict["scsiid"] = id.zfcp.sanitizeInput(dict["scsiid"])
        dict["scsilun"] = id.zfcp.sanitizeHexInput(dict["scsilun"])
        dict["wwpn"] = id.zfcp.sanitizeFCPLInput(dict["wwpn"])

        if id.zfcp.checkValidDevice(dict["devnum"]) == -1:
            raise KickstartValueError, "Invalid devnum specified"
        if id.zfcp.checkValidID(dict["scsiid"]) == -1:
            raise KickstartValueError, "Invalid scsiid specified"
        if id.zfcp.checkValid64BitHex(dict["wwpn"]) == -1:
            raise KickstartValueError, "Invalid wwpn specified"
        if id.zfcp.checkValidID(dict["scsilun"]) == -1:
            raise KickstartValueError, "Invalid scsilun specified"
        if id.zfcp.checkValid64BitHex(dict["fcplun"]) == -1:
            raise KickstartValueError, "Invalid fcplun specified"

        id.instClass.setZFCP(id, dict["devnum"], dict["scsiid"], dict["wwpn"],
                             dict["scsilun"], dict["fcplun"])
        self.skipSteps.append("zfcpconfig")

class VNCHandlers(KickstartHandlers):
    # We're only interested in the handler for the VNC command.
    def __init__ (self, ksdata):
        KickstartHandlers.__init__(self, ksdata)
        self.resetHandlers()
        self.handlers["vnc"] = self.doVnc

class KickstartPreParser(KickstartParser):
    def __init__ (self, ksdata, kshandlers):
        self.handler = kshandlers
        KickstartParser.__init__(self, ksdata, kshandlers,
                                 missingIncludeIsFatal=False)

    def addScript (self):
        if self.state == STATE_PRE:
            s = AnacondaKSScript (self.script["body"], self.script["interp"],
			          self.script["chroot"], self.script["log"],
				  self.script["errorOnFail"])
            self.ksdata.scripts.append(s)

    def addPackages (self, line):
        pass

    def handleCommand (self, lineno, args):
        pass

    def handlePackageHdr (self, lineno, args):
        pass

    def handleScriptHdr (self, lineno, args):
        if not args[0] == "%pre":
            return

        op = KSOptionParser(lineno=lineno)
        op.add_option("--erroronfail", dest="errorOnFail", action="store_true",
                      default=False)
        op.add_option("--interpreter", dest="interpreter", default="/bin/sh")
        op.add_option("--log", "--logfile", dest="log")

        (opts, extra) = op.parse_args(args=args[1:])

        self.script["interp"] = opts.interpreter
        self.script["log"] = opts.log
        self.script["errorOnFail"] = opts.errorOnFail
        self.script["chroot"] = False

class AnacondaKSParser(KickstartParser):
    def __init__ (self, ksdata, kshandlers, id):
        self.id = id
        KickstartParser.__init__(self, ksdata, kshandlers)

    # Map old broken Everything group to the new futuristic package globs
    def addPackages (self, line):
        if line[0] == '@' and line[1:].lower().strip() == "everything":
            warnings.warn("The Everything group syntax is deprecated.  It may be removed from future releases, which will result in an error from kickstart.  Please use an asterisk on its own line instead.", DeprecationWarning)
            KickstartParser.addPackages(self, "*")
        else:
            KickstartParser.addPackages(self, line)

    def addScript (self):
        if string.join(self.script["body"]).strip() == "":
            return

        s = AnacondaKSScript (self.script["body"], self.script["interp"],
                              self.script["chroot"], self.script["log"],
                              self.script["errorOnFail"], self.script["type"])

        self.ksdata.scripts.append(s)

    def handleCommand (self, lineno, args):
        if not self.handler:
            return

        cmd = args[0]
        cmdArgs = args[1:]

        if not self.handler.handlers.has_key(cmd):
            raise KickstartParseError, formatErrorMsg(lineno)
        else:
            if self.handler.handlers[cmd] != None:
                self.handler.currentCmd = cmd
                self.handler.lineno = lineno
                self.handler.handlers[cmd](self.id, cmdArgs)

# The anaconda kickstart processor.
class Kickstart(BaseInstallClass):
    name = "kickstart"

    def __init__(self, file, serial):
        self.ksdata = None
        self.handlers = None
        self.serial = serial
        self.file = file

        BaseInstallClass.__init__(self, 0)

    # this adds a partition to the autopartition list replacing anything
    # else with this mountpoint so that you can use autopart and override /
    def addPartRequest(self, partitions, request):
        if not request.mountpoint:
            partitions.autoPartitionRequests.append(request)
            return

        for req in partitions.autoPartitionRequests:
            if req.mountpoint and req.mountpoint == request.mountpoint:
                partitions.autoPartitionRequests.remove(req)
                break
        partitions.autoPartitionRequests.append(request)            

    def runPreScripts(self, intf = None):
        preScripts = filter (lambda s: s.type == KS_SCRIPT_PRE,
                             self.ksdata.scripts)

        if len(preScripts) == 0:
            return

	log.info("Running kickstart %%pre script(s)")
        if intf is not None:
            w = intf.waitWindow(_("Running..."),
                                _("Running pre-install scripts"))
        
        map (lambda s: s.run("/", self.serial, intf), preScripts)

	log.info("All kickstart %%pre script(s) have been run")
        if intf is not None:
            w.pop()

    def postAction(self, rootPath, serial, intf = None):
        postScripts = filter (lambda s: s.type == KS_SCRIPT_POST,
                              self.ksdata.scripts)

        if len(postScripts) == 0:
            return

	log.info("Running kickstart %%post script(s)")
        if intf is not None:
            w = intf.waitWindow(_("Running..."),
                                _("Running post-install scripts"))
            
        map (lambda s: s.run(rootPath, serial, intf), postScripts)

	log.info("All kickstart %%post script(s) have been run")
        if intf is not None:
            w.pop()

    def runTracebackScripts(self):
	log.info("Running kickstart %%traceback script(s)")
	for script in filter (lambda s: s.type == KS_SCRIPT_TRACEBACK,
                              self.ksdata.scripts):
	    script.run("/", self.serial)
        log.info("All kickstart %%traceback script(s) have been run")

    def setInstallData (self, id, intf = None):
        BaseInstallClass.setInstallData(self, id)
        self.setEarlySwapOn(1)
        self.id = id
        self.id.firstboot = FIRSTBOOT_SKIP

        # make sure our disks are alive
        from partedUtils import DiskSet
        ds = DiskSet()
        ds.startDmRaid()

        # parse the %pre
        self.ksdata = KickstartData()
        self.ksparser = KickstartPreParser(self.ksdata, None)

        try:
            self.ksparser.readKickstart(self.file)
        except KickstartError, e:
           if intf:
               intf.kickstartErrorWindow(e.__str__())
               sys.exit(0)
           else:
               raise KickstartError, e

        # run %pre scripts
        self.runPreScripts(intf)

        # now read the kickstart file for real
        self.ksdata = KickstartData()
        self.handlers = AnacondaKSHandlers(self.ksdata)
        self.ksparser = AnacondaKSParser(self.ksdata, self.handlers, self.id)

        try:
            self.ksparser.readKickstart(self.file)
        except KickstartError, e:
            if intf:
                intf.kickstartErrorWindow(e.__str__())
                sys.exit(0)
            else:
                raise KickstartError, e

    def setSteps(self, dispatch):
        if self.ksdata.upgrade:
            from upgradeclass import InstallClass
            theUpgradeclass = InstallClass(0)
            theUpgradeclass.setSteps(dispatch)

            # we have no way to specify migrating yet
            dispatch.skipStep("upgrademigfind")
            dispatch.skipStep("upgrademigratefs")
            dispatch.skipStep("upgradecontinue")
            dispatch.skipStep("findinstall", permanent = 1)
            dispatch.skipStep("language")
            dispatch.skipStep("keyboard")
            dispatch.skipStep("welcome")
            dispatch.skipStep("betanag")
            dispatch.skipStep("installtype")
        else:
            BaseInstallClass.setSteps(self, dispatch)
            dispatch.skipStep("findrootparts")

        if self.ksdata.interactive or flags.autostep:
            dispatch.skipStep("installtype")
            dispatch.skipStep("bootdisk")

        # because these steps depend on the monitor being probed
        # properly, and will stop you if you have an unprobed monitor,
        # we should skip them for autostep
        if flags.autostep:
            dispatch.skipStep("monitor")
            return

        dispatch.skipStep("bootdisk")
        dispatch.skipStep("welcome")
        dispatch.skipStep("betanag")
        dispatch.skipStep("installtype")
        dispatch.skipStep("tasksel")            

        # Don't show confirmation screens in interactive.
        if not self.ksdata.interactive:
            dispatch.skipStep("confirminstall")
            dispatch.skipStep("confirmupgrade")

        # Make sure to automatically reboot even in interactive if told to.
        if self.ksdata.interactive and self.ksdata.reboot["action"] != KS_WAIT:
            dispatch.skipStep("complete")

        # If the package section included anything, skip group selection unless
        # they're in interactive.
        if self.ksdata.upgrade:
            self.handlers.skipSteps.append("group-selection")
	elif len(self.ksdata.groupList) > 0 or len(self.ksdata.packageList) > 0 or \
           len(self.ksdata.excludedList) > 0:
            if self.ksdata.interactive:
                self.handlers.showSteps.append("group-selection")
            else:
                self.handlers.skipSteps.append("group-selection")
        else:
            self.handlers.showSteps.append("group-selection")

        if not self.ksdata.interactive:
            for n in self.handlers.skipSteps:
                dispatch.skipStep(n)
            for n in self.handlers.permanentSkipSteps:
                dispatch.skipStep(n, permanent=1)
        for n in self.handlers.showSteps:
            dispatch.skipStep(n, skip = 0)

    def setPackageSelection(self, backend, intf=None, *args):
        for pkg in self.ksdata.packageList:
            num = backend.selectPackage(pkg)
            if self.ksdata.handleMissing == KS_MISSING_IGNORE:
                continue
            if num > 0:
                continue
            rc = intf.messageWindow(_("Missing Package"),
                                    _("You have specified that the "
                                      "package '%s' should be installed.  "
                                      "This package does not exist. "
                                      "Would you like to continue or "
                                      "abort your installation?") %(pkg,),
                                    type="custom",
                                    custom_buttons=[_("_Abort"),
                                                    _("_Continue")])
            if rc == 0:
                sys.exit(1)
            else:
                pass

    def setGroupSelection(self, backend, intf=None, *args):
        backend.selectGroup("Core")

        if self.ksdata.addBase:
            backend.selectGroup("Base")
        else:
            log.warning("not adding Base group")

        # FIXME: handling of missing groups
        for grp in self.ksdata.groupList:
            num = backend.selectGroup(grp)
            if self.ksdata.handleMissing == KS_MISSING_IGNORE:
                continue
            if num > 0:
                continue
            rc = intf.messageWindow(_("Missing Group"),
                                    _("You have specified that the "
                                      "group '%s' should be installed. "
                                      "This group does not exist. "
                                      "Would you like to continue or "
                                      "abort your installation?")
                                    %(grp,),
                                    type="custom",
                                    custom_buttons=[_("_Abort"),
                                                    _("_Continue")])
            if rc == 0:
                sys.exit(1)
            else:
                pass



        # FIXME: need to handle package exclusions here.
        map(backend.deselectPackage, self.ksdata.excludedList)

#
# look through ksfile and if it contains a line:
#
# %ksappend <url>
#
# pull <url> down and append to /tmp/ks.cfg. This is run before we actually
# parse the complete kickstart file.
#
# Main use is to have the ks.cfg you send to the loader be minimal, and then
# use %ksappend to pull via https anything private (like passwords, etc) in
# the second stage.
#
def pullRemainingKickstartConfig(ksfile):
    try:
	f = open(ksfile, "r")
    except:
	raise KickstartError ("Unable to open ks file %s for append" % ksfile)

    lines = f.readlines()
    f.close()

    url = None
    for l in lines:
	ll = l.strip()
	if string.find(ll, "%ksappend") == -1:
	    continue

	try:
	    (xxx, ksurl) = string.split(ll, ' ')
	except:
	    raise KickstartError ("Illegal url for %%ksappend - %s" % ll)

	log.info("Attempting to pull second part of ks.cfg from url %s" % ksurl)

	try:
	    url = grabber.urlopen (ksurl)
	except grabber.URLGrabError, e:
	    raise KickstartError ("IOError: %s" % e.strerror)
	else:
	    # sanity check result - sometimes FTP doesnt
	    # catch a file is missing
	    try:
		clen = url.info()['content-length']
	    except Exception, e:
		clen = 0

	    if clen < 1:
		raise KickstartError ("IOError: -1:File not found")

        break

    # if we got something then rewrite /tmp/ks.cfg with new information
    if url is not None:
	os.rename("/tmp/ks.cfg", "/tmp/ks.cfg-part1")

	# insert contents of original /tmp/ks.cfg w/o %ksappend line
	f = open("/tmp/ks.cfg", 'w+')
	for l in lines:
	    ll = l.strip()
	    if string.find(ll, "%ksappend") != -1:
		continue
	    f.write(l)

	# now write part we just grabbed
	f.write(url.read())
	f.close()

	# close up url and we're done
	url.close()
	
    return None

