#
# upgrade_bootloader_gui.py: gui bootloader dialog for upgrades
#
# Jeremy Katz <katzj@redhat.com>
#
# Copyright, 2002, 2007 Red Hat, Inc.
#
#
# This software may be freely redistributed under the terms of the GNU
# library public license.
#
# You should have received a copy of the GNU Library Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#

# must replace with explcit form so update disks will work
from iw_gui import *

import gtk
from rhpl.translate import _, N_
import iutil
import gui
import checkbootloader

class UpgradeBootloaderWindow (InstallWindow):
    windowTitle = N_("Upgrade Boot Loader Configuration")

    def getPrev(self):
        pass

    def getNext(self):
        if self.nobl_radio.get_active():
            self.dispatch.skipStep("bootloadersetup", skip = 1)
            self.dispatch.skipStep("bootloader", skip = 1)
            self.dispatch.skipStep("bootloaderadvanced", skip = 1)
            self.dispatch.skipStep("instbootloader", skip = 1)
        elif self.newbl_radio.get_active():
            self.dispatch.skipStep("bootloadersetup", skip = 0)
            self.dispatch.skipStep("bootloader", skip = 0)
            self.dispatch.skipStep("bootloaderadvanced", skip = 0)
            self.dispatch.skipStep("instbootloader", skip = 0)
            self.bl.doUpgradeOnly = 0
        else:
            self.dispatch.skipStep("bootloadersetup", skip = 0)
            self.dispatch.skipStep("bootloader", skip = 1)
            self.dispatch.skipStep("bootloaderadvanced", skip = 1)
            self.dispatch.skipStep("instbootloader", skip = 0)
            self.bl.doUpgradeOnly = 1

            if self.type == "GRUB":
                self.bl.useGrubVal = 1
            else:
                self.bl.useGrubVal = 0
            self.bl.setDevice(self.bootDev)

    def _newToLibata(self, rootPath):
        # NOTE: any changes here need to be done in upgrade_bootloader_text too
        try:
            f = open("/proc/modules", "r")
            buf = f.read()
            if buf.find("libata") == -1:
                return False
        except:
            log.debug("error reading /proc/modules")
            pass

        try:
            f = open(rootPath + "/etc/modprobe.conf")
        except:
            log.debug("error reading /etc/modprobe.conf")
            return False

        lines = f.readlines()
        f.close()

        for l in lines:
            stripped = l.strip()

            if stripped == "" or stripped[0] == "#":
                continue

            if stripped.find("scsi_hostadapter") == -1:
                return True

        return False

    def getScreen(self, anaconda):
        self.dispatch = anaconda.dispatch
        self.bl = anaconda.id.bootloader

        newToLibata = self._newToLibata(anaconda.rootPath)

        (self.type, self.bootDev) = \
                    checkbootloader.getBootloaderTypeAndBoot(anaconda.rootPath)

        self.update_radio = gtk.RadioButton(None, _("_Update boot loader configuration"))
        updatestr = _("This will update your current boot loader.")

        if newToLibata or (self.type is None or self.bootDev is None):
            if newToLibata:
                current = _("Due to system changes, your boot loader "
                            "configuration can not be automatically updated.")
            else:
                current = _("The installer is unable to detect the boot loader "
                            "currently in use on your system.")
            self.update_label = gtk.Label("%s" % (updatestr,))
            self.update_radio.set_sensitive(False)
            self.update_label.set_sensitive(False)
            update = 0
        else:
            current = _("The installer has detected the %s boot loader "
                        "currently installed on %s.") % (self.type,
                                                         self.bootDev)
            self.update_label = gtk.Label("%s  %s" % (updatestr,
                                         _("This is the recommended option.")))
            self.update_radio.set_active(False)
            update = 1

        self.newbl_radio = gtk.RadioButton(self.update_radio,
                                          _("_Create new boot loader "
                                            "configuration"))
        self.newbl_label = gtk.Label(_("This option creates a "
                                      "new boot loader configuration.  If "
                                      "you wish to switch boot loaders, you "
                                      "should choose this."))

        self.newbl_radio.set_active(False)
        self.nobl_radio = gtk.RadioButton(self.update_radio,
                                         _("_Skip boot loader updating"))
        self.nobl_label = gtk.Label(_("This option makes no changes to boot "
                                     "loader configuration.  If you are "
                                     "using a third party boot loader, you "
                                     "should choose this."))
        self.nobl_radio.set_active(False)

        for label in [self.update_label, self.nobl_label, self.newbl_label]:
            label.set_alignment(0.8, 0)
            label.set_size_request(275, -1)
            label.set_line_wrap(True)

        str = _("What would you like to do?")
        # if they have one, the default is to update, otherwise the
        # default is to not touch anything
        if update == 1:
            default = self.update_radio
        elif newToLibata:
            default = self.newbl_radio
        else:
            default = self.nobl_radio

        if not self.dispatch.stepInSkipList("bootloader"):
            self.newbl_radio.set_active(True)
        elif self.dispatch.stepInSkipList("instbootloader"):
            self.nobl_radio.set_active(True)
        else:
            default.set_active(True)

        box = gtk.VBox(False, 5)

        label = gtk.Label(current)
        label.set_line_wrap(True)
        label.set_alignment(0.5, 0.0)
        label.set_size_request(300, -1)
        label2 = gtk.Label(str)
        label2.set_line_wrap(True)
        label2.set_alignment(0.5, 0.0)
        label2.set_size_request(300, -1)

        box.pack_start(label, False)
        box.pack_start(label2, False, padding = 10)

        box.pack_start(self.update_radio, False)
        box.pack_start(self.update_label, False)
        box.pack_start(self.nobl_radio, False)
        box.pack_start(self.nobl_label, False)
        box.pack_start(self.newbl_radio, False)
        box.pack_start(self.newbl_label, False)

        a = gtk.Alignment(0.2, 0.1)
        a.add(box)

        return a
