# Base classes for the graphical user interface.
#
# Copyright (C) 2011-2012  Red Hat, Inc.
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
# Red Hat Author(s): Chris Lumens <clumens@redhat.com>
#
import inspect, os, sys, time, site
import meh.ui.gui

from gi.repository import Gdk, Gtk, AnacondaWidgets, Keybinder

from pyanaconda.i18n import _
from pyanaconda.constants import IPMI_ABORTED
from pyanaconda import product, iutil, constants

from pyanaconda.ui import UserInterface, common
from pyanaconda.ui.gui.utils import enlightbox, gtk_action_wait, gtk_call_once, busyCursor, unbusyCursor
from pyanaconda import ihelp
import os.path

import logging
log = logging.getLogger("anaconda")

__all__ = ["GraphicalUserInterface", "GUIObject", "QuitDialog"]

_screenshotIndex = 0
_last_screenshot_timestamp = 0
SCREENSHOT_DELAY = 1  # in seconds


ANACONDA_WINDOW_GROUP = Gtk.WindowGroup()

class GUIObject(common.UIObject):
    """This is the base class from which all other GUI classes are derived.  It
       thus contains only attributes and methods that are common to everything
       else.  It should not be directly instantiated.

       Class attributes:

       builderObjects   -- A list of UI object names that should be extracted from
                           uiFile and exposed for this class to use.  If this list
                           is empty, all objects will be exposed.

                           Only the following kinds of objects need to be exported:

                           (1) Top-level objects (like GtkDialogs) that are directly
                           used in Python.

                           (2) Top-level objects that are not directly used in
                           Python, but are used by another object somewhere down
                           in the hierarchy.  This includes things like a custom
                           GtkImage used by a button that is part of an exported
                           dialog, and a GtkListStore that is the model of a
                           Gtk*View that is part of an exported object.
       mainWidgetName   -- The name of the top-level widget this object
                           object implements.  This will be the widget searched
                           for in uiFile by the window property.
       uiFile           -- The location of an XML file that describes the layout
                           of widgets shown by this object.  UI files are
                           searched for relative to the same directory as this
                           object's module.
       translationDomain-- The gettext translation domain for the given GUIObject
                           subclass. By default the "anaconda" translation domain
                           is used, but external applications, such as Initial Setup,
                           that use GUI elements (Hubs & Spokes) from Anaconda
                           can override the translation domain with their own,
                           so that their subclasses are properly translated.
       helpFile         -- The location of the yelp-compatible help file for the
                           given GUI object. The default value of "" indicates
                           that the object has not specific help file assigned
                           and the default help file should be used.
    """
    builderObjects = []
    mainWidgetName = None
    uiFile = ""
    helpFile = None
    translationDomain = "anaconda"

    handles_autostep = False

    def __init__(self, data):
        """Create a new UIObject instance, including loading its uiFile and
           all UI-related objects.

           Instance attributes:

           data     -- An instance of a pykickstart Handler object.  The Hub
                       never directly uses this instance.  Instead, it passes
                       it down into Spokes when they are created and applied.
                       The Hub simply stores this instance so it doesn't need
                       to be passed by the user.
           skipTo   -- If this attribute is set to something other than None,
                       it must be the name of a class (as a string).  Then,
                       the interface will skip to the first instance of that
                       class in the action list instead of going on to
                       whatever the next action is normally.

                       Note that actions may only skip ahead, never backwards.
                       Also, standalone spokes may not skip to an individual
                       spoke off a hub.  They can only skip to the hub
                       itself.
        """
        common.UIObject.__init__(self, data)

        if self.__class__ is GUIObject:
            raise TypeError("GUIObject is an abstract class")

        self.skipTo = None
        self.applyOnSkip = False

        self.builder = Gtk.Builder()
        self.builder.set_translation_domain(self.translationDomain)
        self._window = None

        if self.builderObjects:
            self.builder.add_objects_from_file(self._findUIFile(), self.builderObjects)
        else:
            self.builder.add_from_file(self._findUIFile())

        ANACONDA_WINDOW_GROUP.add_window(self.window)
        self.builder.connect_signals(self)

        # Keybinder from GI needs to be initialized before use
        Keybinder.init()
        Keybinder.bind("<Shift>Print", self._handlePrntScreen, [])

        self._automaticEntry = False
        self._autostepRunning = False
        self._autostepDone = False

    def _findUIFile(self):
        path = os.environ.get("UIPATH", "./:/tmp/updates/:/tmp/updates/ui/:/usr/share/anaconda/ui/")
        dirs = path.split(":")

        # append the directory where this UIObject is defined
        dirs.append(os.path.dirname(inspect.getfile(self.__class__)))

        for d in dirs:
            testPath = os.path.join(d, self.uiFile)
            if os.path.isfile(testPath) and os.access(testPath, os.R_OK):
                return testPath

        raise IOError("Could not load UI file '%s' for object '%s'" % (self.uiFile, self))

    def _handlePrntScreen(self, *args, **kwargs):
        global _last_screenshot_timestamp
        # as a single press of the assigned key generates
        # multiple callbacks, we need to skip additional
        # callbacks for some time once a screenshot is taken
        if (time.time() - _last_screenshot_timestamp) >= SCREENSHOT_DELAY:
            self.take_screenshot()
            # start counting from the time the screenshot operation is done
            _last_screenshot_timestamp = time.time()

    def take_screenshot(self, name=None):
        """Take a screenshot of the whole screen (works even with multiple displays)

        :param name: optional name for the screenshot that will be appended to the filename,
                     after the standard prefix & screenshot number
        :type name: str or NoneType
        """
        global _screenshotIndex
        # Make sure the screenshot directory exists.
        iutil.mkdirChain(constants.SCREENSHOTS_DIRECTORY)

        if name is None:
            screenshot_filename = "screenshot-%04d.png" % _screenshotIndex
        else:
            screenshot_filename = "screenshot-%04d-%s.png" % (_screenshotIndex, name)

        fn = os.path.join(constants.SCREENSHOTS_DIRECTORY, screenshot_filename)

        root_window = Gdk.get_default_root_window()
        pixbuf = Gdk.pixbuf_get_from_window(root_window, 0, 0,
                                            root_window.get_width(),
                                            root_window.get_height())
        pixbuf.savev(fn, 'png', [], [])
        log.info("%s taken", screenshot_filename)
        _screenshotIndex += 1

    @property
    def automaticEntry(self):
        """Report if the given GUIObject has been displayed under automatic control

        This is needed for example for installations with an incomplete kickstart,
        as we need to differentiate the automatic screenshot pass from the user
        entering a spoke to manually configure things. We also need to skip applying
        changes if the spoke is entered automatically.
        """
        return self._automaticEntry

    @automaticEntry.setter
    def automaticEntry(self, value):
        self._automaticEntry = value

    @property
    def autostepRunning(self):
        """Report if the GUIObject is currently running autostep"""
        return self._autostepRunning

    @autostepRunning.setter
    def autostepRunning(self, value):
        self._autostepRunning = value

    @property
    def autostepDone(self):
        """Report if autostep for this GUIObject has been finished"""
        return self._autostepDone

    @autostepDone.setter
    def autostepDone(self, value):
        self._autostepDone = value

    def autostep(self):
        """Autostep through this graphical object and through
        any graphical objects managed by it (such as through spokes for a hub)
        """
        # report that autostep is running to prevent another from starting
        self.autostepRunning = True
        # take a screenshot of the current graphical object
        if self.data.autostep.autoscreenshot:
            # as autostep is triggered just before leaving a screen,
            # we can safely take a screenshot of the "parent" object at once
            # without using idle_add
            self.take_screenshot(self.__class__.__name__)
        self._doAutostep()
        # done
        self.autostepRunning = False
        self.autostepDone = True
        self._doPostAutostep()

    def _doPostAutostep(self):
        """To be overridden by the given GUIObject sub-class with custom code
        that brings the GUI from the autostepping mode back to the normal mode.
        This usually means to "click" the continue button or its equivalent.
        """
        pass

    def _doAutostep(self):
        """To be overridden by the given GUIObject sub-class with customized
        autostepping code - if needed
        (this is for example used to step through spokes in a hub)
        """
        pass

    @property
    def window(self):
        """Return the top-level object out of the GtkBuilder representation
           previously loaded by the load method.
        """

        # This will raise an AttributeError if the subclass failed to set a
        # mainWidgetName attribute, which is exactly what I want.
        if not self._window:
            self._window = self.builder.get_object(self.mainWidgetName)

        return self._window

    def clear_info(self):
        """Clear any info bar from the bottom of the screen."""
        self.window.clear_info()

    def set_error(self, msg):
        """Display an info bar along the bottom of the screen with the provided
           message.  This method is used to display critical errors anaconda
           may not be able to do anything about, but that the user may.  A
           suitable background color and icon will be displayed.
        """
        self.window.set_error(msg)

    def set_info(self, msg):
        """Display an info bar along the bottom of the screen with the provided
           message.  This method is used to display informational text -
           non-critical warnings during partitioning, for instance.  The user
           should investigate these messages but doesn't have to.  A suitable
           background color and icon will be displayed.
        """
        self.window.set_info(msg)

    def set_warning(self, msg):
        """Display an info bar along the bottom of the screen with the provided
           message.  This method is used to display errors the user needs to
           attend to in order to continue installation.  This is the bulk of
           messages.  A suitable background color and icon will be displayed.
        """
        self.window.set_warning(msg)

class QuitDialog(GUIObject):
    builderObjects = ["quitDialog"]
    mainWidgetName = "quitDialog"
    uiFile = "main.glade"

    MESSAGE = ""

    def run(self):
        if self.MESSAGE:
            self.builder.get_object("quit_message").set_label(_(self.MESSAGE))
        rc = self.window.run()
        return rc

class ErrorDialog(GUIObject):
    builderObjects = ["errorDialog", "errorTextBuffer"]
    mainWidgetName = "errorDialog"
    uiFile = "main.glade"

    # pylint: disable-msg=W0221
    def refresh(self, msg):
        buf = self.builder.get_object("errorTextBuffer")
        buf.set_text(msg, -1)

    def run(self):
        rc = self.window.run()
        return rc

class GraphicalUserInterface(UserInterface):
    """This is the standard GTK+ interface we try to steer everything to using.
       It is suitable for use both directly and via VNC.
    """
    def __init__(self, storage, payload, instclass,
                 distributionText = product.distributionText, isFinal = product.isFinal,
                 quitDialog = QuitDialog):

        UserInterface.__init__(self, storage, payload, instclass)

        self._actions = []
        self._currentAction = None
        self._ui = None

        self.data = None

        self._distributionText = distributionText
        self._isFinal = isFinal
        self._quitDialog = quitDialog
        self._mehInterface = GraphicalExceptionHandlingIface(
                                    self.lightbox_over_current_action)

    basemask = "pyanaconda.ui.gui"
    basepath = os.path.dirname(__file__)
    updatepath = "/tmp/updates/pyanaconda/ui/gui"
    sitepackages = [os.path.join(dir, "pyanaconda", "ui", "gui")
                    for dir in site.getsitepackages()]
    pathlist = set([updatepath, basepath] + sitepackages)

    paths = UserInterface.paths + {
            "categories": [(basemask + ".categories.%s",
                        os.path.join(path, "categories"))
                        for path in pathlist],
            "spokes": [(basemask + ".spokes.%s",
                        os.path.join(path, "spokes"))
                        for path in pathlist],
            "hubs": [(basemask + ".hubs.%s",
                      os.path.join(path, "hubs"))
                      for path in pathlist]
            }

    @property
    def tty_num(self):
        return 6

    @property
    def meh_interface(self):
        return self._mehInterface

    def _list_hubs(self):
        """Return a list of Hub classes to be imported to this interface"""
        from .hubs.summary import SummaryHub
        from .hubs.progress import ProgressHub
        return [SummaryHub, ProgressHub]

    def _is_standalone(self, obj):
        """Is the spoke passed as obj standalone?"""
        from .spokes import StandaloneSpoke
        return isinstance(obj, StandaloneSpoke)

    def setup(self, data):
        busyCursor()

        self._actions = self.getActionClasses(self._list_hubs())
        self.data = data

    def getActionClasses(self, hubs):
        """Grab all relevant standalone spokes, add them to the passed
           list of hubs and order the list according to the
           relationships between hubs and standalones."""
        from .spokes import StandaloneSpoke

        # First, grab a list of all the standalone spokes.
        standalones = self._collectActionClasses(self.paths["spokes"], StandaloneSpoke)

        # Second, order them according to their relationship
        return self._orderActionClasses(standalones, hubs)

    def lightbox_over_current_action(self, window):
        """
        Creates lightbox over current action for the given window. Or
        DOES NOTHING IF THERE ARE NO ACTIONS.

        """
        # if there are no actions (not populated yet), we can do nothing
        if len(self._actions) > 0 and self._currentAction:
            lightbox = AnacondaWidgets.lb_show_over(self._currentAction.window)
            ANACONDA_WINDOW_GROUP.add_window(lightbox)
            window.main_window.set_transient_for(lightbox)

    def _instantiateAction(self, actionClass):
        from pyanaconda.ui.gui.spokes import StandaloneSpoke

        # Instantiate an action on-demand, passing the arguments defining our
        # spoke API and setting up continue/quit signal handlers.
        obj = actionClass(self.data, self.storage, self.payload, self.instclass)

        # set spoke search paths in Hubs
        if hasattr(obj, "set_path"):
            obj.set_path("spokes", self.paths["spokes"])
            obj.set_path("categories", self.paths["categories"])

        # If we are doing a kickstart install, some standalone spokes
        # could already be filled out.  In that case, we do not want
        # to display them.
        if self._is_standalone(obj) and obj.completed:
            del(obj)
            return None

        obj.register_event_cb("continue", self._on_continue_clicked)
        obj.register_event_cb("quit", self._on_quit_clicked)
        obj.register_event_cb("help-button", self._on_help_clicked, obj)

        return obj

    def run(self):
        (success, args) = Gtk.init_check(None)
        if not success:
            raise RuntimeError("Failed to initialize Gtk")

        if Gtk.main_level() > 0:
            # Gtk main loop running. That means python-meh caught exception
            # and runs its main loop. Do not crash Gtk by running another one
            # from a different thread and just wait until python-meh is
            # finished, then quit.
            unbusyCursor()
            log.error("Unhandled exception caught, waiting for python-meh to "\
                      "exit")
            while Gtk.main_level() > 0:
                time.sleep(2)

            sys.exit(0)

        while not self._currentAction:
            self._currentAction = self._instantiateAction(self._actions[0])
            if not self._currentAction:
                self._actions.pop(0)

            if not self._actions:
                return

        self._currentAction.initialize()
        self._currentAction.refresh()

        self._currentAction.window.set_beta(not self._isFinal)
        self._currentAction.window.set_property("distribution", self._distributionText().upper())

        # Set some program-wide settings.
        settings = Gtk.Settings.get_default()
        settings.set_property("gtk-font-name", "Cantarell")
        settings.set_property("gtk-icon-theme-name", "gnome")

        # Apply the application stylesheet
        provider = Gtk.CssProvider()
        provider.load_from_path("/usr/share/anaconda/anaconda-gtk.css")
        Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), provider,
                Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

        self._currentAction.window.show_all()

        # Do this at the last possible minute.
        unbusyCursor()

        Gtk.main()

    ###
    ### MESSAGE HANDLING METHODS
    ###
    @gtk_action_wait
    def showError(self, message):
        dlg = ErrorDialog(None)

        with enlightbox(self._currentAction.window, dlg.window):
            dlg.refresh(message)
            dlg.run()
            dlg.window.destroy()

        # the dialog has the only button -- "Exit installer", so just do so
        sys.exit(1)

    @gtk_action_wait
    def showDetailedError(self, message, details):
        from pyanaconda.ui.gui.spokes.lib.detailederror import DetailedErrorDialog
        dlg = DetailedErrorDialog(None, buttons=[_("_Quit")],
                                  label=message)

        with enlightbox(self._currentAction.window, dlg.window):
            dlg.refresh(details)
            rc = dlg.run()
            dlg.window.destroy()

    @gtk_action_wait
    def showYesNoQuestion(self, message):
        dlg = Gtk.MessageDialog(flags=Gtk.DialogFlags.MODAL,
                                message_type=Gtk.MessageType.QUESTION,
                                buttons=Gtk.ButtonsType.NONE,
                                message_format=message)
        dlg.set_decorated(False)
        dlg.add_buttons(_("_No"), 0, _("_Yes"), 1)
        dlg.set_default_response(1)

        with enlightbox(self._currentAction.window, dlg):
            rc = dlg.run()
            dlg.destroy()

        return bool(rc)

    ###
    ### SIGNAL HANDLING METHODS
    ###
    def _on_continue_clicked(self):
        # Autostep needs to be triggered just before switching to the next screen
        # (or before quiting the installation if there are no more screens) to be consistent
        # in both fully automatic kickstart installation and for installation with an incomplete
        # kickstart. Therefore we basically "hook" the continue-clicked signal, start autostepping
        # and ignore any other continue-clicked signals until autostep is done.
        # Once autostep finishes, it emits the appropriate continue-clicked signal itself,
        # switching to the next screen (if any).
        if self.data.autostep.seen and self._currentAction.handles_autostep:
            if self._currentAction.autostepRunning:
                log.debug("ignoring the continue-clicked signal - autostep is running")
                return
            elif not self._currentAction.autostepDone:
                self._currentAction.autostep()
                return

        # If we're on the last screen, clicking Continue quits.
        if len(self._actions) == 1:
            # save the screenshots to the installed system before killing Anaconda
            # (the kickstart post scripts run to early, so we need to copy the screenshots now)
            iutil.save_screenshots()
            Gtk.main_quit()
            return

        nextAction = None
        ndx = 0

        # If the current action wants us to jump to an arbitrary point ahead,
        # look for where that is now.
        if self._currentAction.skipTo:
            found = False
            for ndx in range(1, len(self._actions)):
                if self._actions[ndx].__class__.__name__ == self._currentAction.skipTo:
                    found = True
                    break

            # If we found the point in question, compose a new actions list
            # consisting of the current action, the one to jump to, and all
            # the ones after.  That means the rest of the code below doesn't
            # have to change.
            if found:
                self._actions = [self._actions[0]] + self._actions[ndx:]

        # _instantiateAction returns None for actions that should not be
        # displayed (because they're already completed, for instance) so skip
        # them here.
        while not nextAction:
            nextAction = self._instantiateAction(self._actions[1])
            if not nextAction:
                self._actions.pop(1)

            if not self._actions:
                sys.exit(0)
                return

        nextAction.initialize()
        nextAction.window.set_beta(self._currentAction.window.get_beta())
        nextAction.window.set_property("distribution", self._distributionText().upper())

        if not nextAction.showable:
            self._currentAction.window.hide()
            self._actions.pop(0)
            self._on_continue_clicked()
            return

        nextAction.refresh()

        # Do this last.  Setting up curAction could take a while, and we want
        # to leave something on the screen while we work.
        nextAction.window.show_all()
        self._currentAction.window.hide()
        self._currentAction = nextAction
        self._actions.pop(0)

    def _on_help_clicked(self, window, obj):
        # the help button has been clicked, start the yelp viewer with
        # content for the current screen
        ihelp.start_yelp(ihelp.get_help_path(obj.helpFile, self.instclass))

    def _on_quit_clicked(self):
        dialog = self._quitDialog(None)
        with enlightbox(self._currentAction.window, dialog.window):
            rc = dialog.run()
            dialog.window.destroy()

        if rc == 1:
            iutil.ipmi_report(IPMI_ABORTED)
            sys.exit(0)

class GraphicalExceptionHandlingIface(meh.ui.gui.GraphicalIntf):
    """
    Class inheriting from python-meh's GraphicalIntf and overriding methods
    that need some modification in Anaconda.

    """

    def __init__(self, lightbox_func):
        """
        :param lightbox_func: a function that creates lightbox for a given
                              window
        :type lightbox_func: GtkWindow -> None

        """
        meh.ui.gui.GraphicalIntf.__init__(self)

        self._lightbox_func = lightbox_func

    def mainExceptionWindow(self, text, exn_file, *args, **kwargs):
        meh_intf = meh.ui.gui.GraphicalIntf()
        exc_window = meh_intf.mainExceptionWindow(text, exn_file)
        exc_window.main_window.set_decorated(False)

        self._lightbox_func(exc_window)

        # without a new GtkWindowGroup, python-meh's window is insensitive if it
        # appears above a spoke (Gtk.Window running its own Gtk.main loop)
        window_group = Gtk.WindowGroup()
        window_group.add_window(exc_window.main_window)

        # the busy cursor may be set
        unbusyCursor()

        return exc_window

