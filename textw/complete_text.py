#
# complete_text.py: text mode congratulations windows
#
# Copyright 2001-2002 Red Hat, Inc.
#
# This software may be freely redistributed under the terms of the GNU
# library public license.
#
# You should have received a copy of the GNU Library Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#

from snack import *
from constants_text import *
from rhpl.translate import _
from constants import *
import iutil


class FinishedWindow:
  
  def __call__ (self, screen):
        screen.pushHelpLine (string.center(_("<Enter> to reboot"),
                                           screen.width))

        if iutil.getArch() == "i386":
          bootstr = _("If you created a boot diskette during this "
                      "installation as your primary means of "
                      "booting %s, insert it before "
                      "rebooting your newly installed system.\n\n") % (productName,)
        else:
          bootstr = ""

        if iutil.getArch() == "s390":
          floppystr = _("Press <Enter> to reboot your system.\n\n")
        else:
          floppystr = _("Remove any installation media (diskettes or CD-ROMs) used during the "
                        "installation process and press <Enter> to reboot your system."
                        "\n\n")
          

        rc = ButtonChoiceWindow (screen, _("Complete"), 
             _("Congratulations, your %s installation is "
               "complete.\n\n"
               "%s"
               "%s"
               "For information on errata (updates and bug fixes), visit "
               "http://www.redhat.com/errata.\n\n"
               "Information on using your "
               "system is available in the %s manuals at "
               "http://www.redhat.com/docs.") %
                                 (productName, floppystr, bootstr, productName),
                                 [ _("OK") ], help = "finished", width=60)

        return INSTALL_OK


