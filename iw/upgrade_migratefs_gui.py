#
# upgrade_migratefs_gui.py: dialog for migrating filesystems on upgrades
#
# Mike Fulbright <msf@redhat.com>
#
# Copyright 2001 Red Hat, Inc.
#
# This software may be freely redistributed under the terms of the GNU
# library public license.
#
# You should have received a copy of the GNU Library Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#

from gtk import *
from iw_gui import *
from translate import _, N_
import string
import isys 
import iutil
from log import log
import upgrade
from gnome.ui import *
from fsset import *
import gui

class UpgradeMigrateFSWindow (InstallWindow):		
    windowTitle = N_("Migrate Filesystems")
    htmlTag = "upmigfs"

    def getNext (self):
        for entry in self.migent:
            entry.setFormat(0)
            entry.setMigrate(0)
            entry.fsystem = entry.origfsystem

        for (cb, entry) in self.cbs:
            if cb.get_active():
                entry.setFileSystemType(fileSystemTypeGet("ext3"))
                entry.setFormat(0)
                entry.setMigrate(1)
                
        return None

    def getScreen (self, fsset):
      
        self.migent = fsset.getMigratableEntries()
        self.fsset = fsset
        
        box = GtkVBox (FALSE, 5)
        box.set_border_width (5)

	text = N_("This release of Red Hat Linux supports "
                 "the ext3 journalling filesystem.  It has several "
                 "benefits over the ext2 filesystem traditionally shipped "
                 "in Red Hat Linux.  It is possible to migrate the ext2 "
                 "formatted partitions to ext3 without data loss.\n\n"
                 "Which of these partitions would you like to migrate?")
        
	label = GtkLabel (_text))
        label.set_alignment (0.5, 0.0)
        label.set_usize(400, -1)
        label.set_line_wrap (TRUE)
        box.pack_start(label, FALSE)

        cbox = GtkVBox(FALSE, 5)
        self.cbs = []
        for entry in self.migent:
            if entry.fsystem.getName() != entry.origfsystem.getName():
                migrating = 1
            else:
                migrating = 0
            
            cb = GtkCheckButton("/dev/%s - %s - %s" % (entry.device.getDevice(),
                                              entry.origfsystem.getName(),
                                              entry.mountpoint))
            cb.set_active(migrating)
            cbox.pack_start(cb, FALSE)

            self.cbs.append((cb, entry))

        sw = GtkScrolledWindow()
        sw.set_policy(POLICY_NEVER, POLICY_AUTOMATIC)
        sw.add_with_viewport(cbox)
        sw.set_usize(-1, 175)
        
        viewport = sw.children()[0]
        viewport.set_shadow_type(SHADOW_ETCHED_IN)
        
        a = GtkAlignment(0.25, 0.5)
        a.add(sw)

        box.pack_start(a, TRUE)
        
        a = GtkAlignment(0.5, 0.5)
        a.add(box)
        return a
    
                       
