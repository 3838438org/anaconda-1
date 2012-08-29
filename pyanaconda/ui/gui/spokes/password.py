# root password spoke class
#
# Copyright (C) 2012 Red Hat, Inc.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions of
# the GNU General Public License v.2, or (at your option) any later version.
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY expressed or implied, including the implied warranties of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
# Public License for more details.  You should have received a copy of the
# GNU General Public License along with this program; if not, write to the
# Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301, USA.  Any Red Hat trademarks that are incorporated in the
# source code or documentation are not subject to the GNU General Public
# License and may only be used or replicated with the express permission of
# Red Hat, Inc.
#
# Red Hat Author(s): Jesse Keating <jkeating@redhat.com>
#

import gettext
_ = lambda x: gettext.ldgettext("anaconda", x)
N_ = lambda x: x

from gi.repository import Gtk

from pyanaconda.users import cryptPassword
import pwquality
import string

from pyanaconda.ui.gui.spokes import NormalSpoke
# Need a new category I guess
from pyanaconda.ui.gui.categories.localization import LocalizationCategory
#from _isys import isCapsLockEnabled

__all__ = ["PasswordSpoke"]


class PasswordSpoke(NormalSpoke):
    builderObjects = ["passwordWindow"]

    mainWidgetName = "passwordWindow"
    uiFile = "spokes/password.glade"

    # update this category to something... new?
    category = LocalizationCategory

    icon = "dialog-password-symbolic"
    title = N_("ROOT PASSWORD")

    def __init__(self, *args):
        NormalSpoke.__init__(self, *args)
        self._password = None
        self._error = False
        self._oldweak = None

    def initialize(self):
        NormalSpoke.initialize(self)
        # Set the rootpw to locked by default, setting a password is optional
        self.data.rootpw.lock = True
        # place holders for the text boxes
        self.pw = self.builder.get_object("pw")
        self.confirm = self.builder.get_object("confirm")

    def refresh(self):
#        self.setCapsLockLabel()
        self.pw.grab_focus()

# Caps lock detection isn't hooked up right now
#    def setCapsLockLabel(self):
#        if isCapsLockEnabled():
#            self.capslock.set_text("<b>" + _("Caps Lock is on.") + "</b>")
#            self.capslock.set_use_markup(True)
#        else:
#            self.capslock..set_text("")

    @property
    def status(self):
        if self._error:
            return _("Error setting root password")
        elif self.data.rootpw.lock:
            return _("Root account is disabled")
        else:
            return _("Root password is set")

    def apply(self):
        if self._password:
            self.data.rootpw.password = cryptPassword(self._password)
            self.data.rootpw.isCrypted = True
            self.data.rootpw.lock = False
        else:
            # Blank password case, disable the account
            self.data.rootpw.lock = True
            self.data.rootpw.password = ''
            self.data.rootpw.isCrypted = False

    @property
    def completed(self):
        # FUTURE -- update completed to false if package payload doesn't
        # include firstboot and some environment to run it in
        # We are by default complete, but locked.  If a user attempts to set
        # a password but fails, then we are no longer complete.
        return not self._error

    def _validatePassword(self):
        # Do various steps to validate the password
        # sets self._error to an error string
        # Return True if valid, False otherwise
        pw = self.pw.get_text()
        confirm = self.confirm.get_text()

        # if both pw and confirm are blank, password is disabled.
        if (pw and not confirm) or (confirm and not pw):
            self._error = _("You must enter your root password "
                           "and confirm it by typing it a second "
                           "time to continue.")
            return False

        if pw != confirm:
            self._error = _("The passwords you entered were "
                            "different.  Please try again.")
            return False

        if pw and len(pw) < 6:
            self._error = _("The root password must be at least "
                            "six characters long.")
            return False

        if pw:
            try:
                settings = pwquality.PWQSettings()
                settings.read_config()
                settings.check(pw, None, "root")
            except pwquality.PWQError as (e, msg):
                if pw == self._oldweak:
                    # We got a second attempt with the same weak password
                    pass
                else:
                    self._error = _("You have provided a weak password: %s. "
                                    " Press Back again to use anyway.") % msg
                    self._oldweak = pw
                    return False

        legal = string.digits + string.ascii_letters + string.punctuation + " "
        for letter in pw:
            if letter not in legal:
                self._error = _("Requested password contains "
                                "non-ASCII characters, which are "
                                "not allowed.")
                return False

        # if no errors, clear the info for next time we go into the spoke
        self._password = pw
        self.window.clear_info()
        self._error = False
        return True

    def on_back_clicked(self, button):
        if self._validatePassword():
            self.window.clear_info()
            NormalSpoke.on_back_clicked(self, button)
        else:
            self.window.clear_info()
            self.window.set_info(Gtk.MessageType.WARNING, self._error)
            self.pw.grab_focus()
            self.window.show_all()