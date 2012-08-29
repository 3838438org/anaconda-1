#
# exception.py - general exception formatting and saving
#
# Copyright (C) 2000-2012 Red Hat, Inc.
# All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# Author(s): Chris Lumens <clumens@redhat.com>
#            David Cantrell <dcantrell@redhat.com>
#
from meh.handler import *
from meh.dump import *
import isys
import sys
import os
import shutil
import signal
from flags import flags
import kickstart
import storage.errors
from pyanaconda.constants import ROOT_PATH
from gi.repository import GLib

import logging
log = logging.getLogger("anaconda")

import gettext
_ = lambda x: gettext.ldgettext("anaconda", x)


class AnacondaExceptionHandler(ExceptionHandler):
    def handleException(self, (ty, value, tb), obj):

        def run_handleException_on_idle(args_tuple):
            """
            Helper function with one argument only so that it can be registered
            with GLib.idle_add() to run on idle.

            @param args_tuple: ((ty, value, tb), obj)

            """

            trace, obj = args_tuple
            ty, value, tb = trace

            super(AnacondaExceptionHandler, self).handleException((ty, value, tb),
                                                                  obj)

        if issubclass(ty, storage.errors.StorageError) and value.hardware_fault:
            hw_error_msg = _("The installation was stopped due to what "
                             "seems to be a problem with your hardware. "
                             "The exact error message is:\n\n%s.\n\n "
                             "The installer will now terminate.") % str(value)
            self.intf.showError(hw_error_msg)
            sys.exit(0)
        else:
            if GLib.main_depth() > 0:
                # main loop is running, don't crash it by running another one
                # potentially from a different thread
                GLib.idle_add(run_handleException_on_idle,
                                ((ty, value, tb), obj))
            else:
                super(AnacondaExceptionHandler, self).handleException(
                                                        (ty, value, tb), obj)

    def postWriteHook(self, (ty, value, tb), anaconda):
        # See if /mnt/sysimage is present and put exception there as well
        if os.access("/mnt/sysimage/root", os.X_OK):
            try:
                dest = "/mnt/sysimage/root/%s" % os.path.basename(self.exnFile)
                shutil.copyfile(self.exnFile, dest)
            except:
                log.error("Failed to copy %s to /mnt/sysimage/root" % self.exnFile)
                pass

        # run kickstart traceback scripts (if necessary)
        try:
            kickstart.runTracebackScripts(anaconda.ksdata.scripts)
        except:
            pass

    def runDebug(self, (ty, value, tb)):
        # vtActivate does not work on certain ppc64 machines, so just skip
        # that and continue with the rest of the debugger setup.
        try:
            isys.vtActivate(1)
        except SystemError:
            pass

        pidfl = "/tmp/vncshell.pid"
        if os.path.exists(pidfl) and os.path.isfile(pidfl):
            pf = open(pidfl, "r")
            for pid in pf.readlines():
                if not int(pid) == os.getpid():
                    os.kill(int(pid), signal.SIGKILL)
            pf.close()

        os.open("/dev/console", os.O_RDWR)   # reclaim stdin
        os.dup2(0, 1)                        # reclaim stdout
        os.dup2(0, 2)                        # reclaim stderr
        #   ^
        #   |
        #   +------ dup2 is magic, I tells ya!

        # bring back the echo
        import termios
        si = sys.stdin.fileno()
        attr = termios.tcgetattr(si)
        attr[3] = attr[3] & termios.ECHO
        termios.tcsetattr(si, termios.TCSADRAIN, attr)

        print("\nEntering debugger...")
        print("Use 'continue' command to quit the debugger and get back to "\
              "the main window")
        import pdb
        pdb.post_mortem (tb)
        try:
            isys.vtActivate(6)
        except SystemError:
            pass

def initExceptionHandling(anaconda):
    fileList = [ "/tmp/anaconda.log",
                 "/tmp/program.log", "/tmp/storage.log", "/tmp/ifcfg.log",
                 "/tmp/yum.log", ROOT_PATH + "/root/install.log",
                 ROOT_PATH + "/root/upgrade.log", "/proc/cmdline" ]
    if flags.livecdInstall:
        fileList.extend(["/var/log/messages"])
    else:
        fileList.extend(["/tmp/syslog"])

    if anaconda.opts and anaconda.opts.ksfile:
        fileList.extend([anaconda.opts.ksfile])

    conf = Config(programName="anaconda",
                  programVersion=isys.getAnacondaVersion(),
                  attrSkipList=["_intf._actions",
                                "_intf.storage.bootloader.password",
                                "_intf.storage.data",
                                "_bootloader.encrypted_password",
                                "_bootloader.password",
                                "payload._groups",
                                "payload._yum"],
                  localSkipList=[ "passphrase", "password" ],
                  fileList=fileList)
    handler = AnacondaExceptionHandler(conf, anaconda.intf, ReverseExceptionDump)
    handler.install(anaconda)

    return conf
