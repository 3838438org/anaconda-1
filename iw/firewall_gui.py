#
# firewall_gui.py: firewall setup screen
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

import checklist
import gtk
import gui
from iw_gui import *
from isys import *
from rhpl.translate import _, N_

class FirewallWindow (InstallWindow):		

    windowTitle = N_("Firewall Configuration")
    htmlTag = "securitylevel"

    def __init__ (self, ics):
	InstallWindow.__init__ (self, ics)

    def getNext (self):
        if not self.__dict__.has_key("sec_none_radio"):
            return None

        if self.sec_none_radio.get_active ():
            self.firewall.enabled = 0
            self.firewall.policy = 1
        else:

            if self.sec_high_radio.get_active ():
                self.firewall.policy = 0
                self.firewall.enabled = 1
            elif self.sec_med_radio.get_active ():
                self.firewall.policy = 1
                self.firewall.enabled = 1

            if self.default_radio.get_active ():
                self.firewallState = 0

            if self.custom_radio.get_active ():
                self.firewallState = 1
                count = 0
                self.firewall.trustdevs = []

                for device in self.devices:
                    val = self.trusted.get_active(count)
                    if val == 1:
                        self.firewall.trustdevs.append(device)
                    count = count + 1

                count = 0
                for service in self.knownPorts.keys():
                    val = self.incoming.get_active(count)
                    if service == "DHCP":
                        self.firewall.dhcp = val
                    elif service == "SSH":
                        self.firewall.ssh = val
                    elif service == "Telnet":
                        self.firewall.telnet = val
                    elif service == "WWW (HTTP)":
                        self.firewall.http = val
                    elif service == "Mail (SMTP)":
                        self.firewall.smtp = val
                    elif service == "FTP":
                        self.firewall.ftp = val                    
                    count = count + 1
                    
                portstring = string.strip(self.ports.get_text())
                portlist = ""
                bad_token_found = 0
                bad_token = ""
                if portstring != "":
                    tokens = string.split(portstring, ',')
                    for token in tokens:
                        try:
                            #- if there's a colon in the token, it's valid
                            if string.index(token,':'):         
                                parts = string.split(token, ':')
				try:
				    portnum = string.atoi(parts[0])
				except:
				    portnum = None
				    
                                if len(parts) > 2: # more than one colon
                                    bad_token_found = 1
                                    bad_token = token
				elif portnum is not None and (portnum < 1 or portnum > 65535):
				    bad_token_found = 1
				    bad_token = token
                                else:
                                    # udp and tcp are the only valid protos
                                    if parts[1] == 'tcp' or parts[1] == 'udp':
                                        if portlist == "":
                                            portlist = token
                                        else:
                                            portlist = portlist + ',' + token
                                    else: # found protocol !tcp && !udp
                                        bad_token_found = 1
                                        bad_token = token
                                        pass
                        except:
                            if token != "":
				try:
				    try:
					portnum = string.atoi(token)
				    except:
					portnum = None
				    
				    if portnum is not None and (portnum < 1 or portnum > 65535):
					bad_token_found = 1
					bad_token = token
				    else:
					if portlist == "":
					    portlist = token + ":tcp"
					else:
					    portlist = portlist + ',' + token + ':tcp'
				except:
				    bad_token_found = 1
				    bad_token = token
                else:
                    pass

                if bad_token_found == 1: # raise a warning
                    text = _("Invalid port given: %s.  The proper format is "
                             "'port:protocol', where port is between 1 and 65535, and protocol is either 'tcp' or 'udp'.\n\nFor example, "
                             "'1234:udp'") % (bad_token,)

                    self.intf.messageWindow(_("Warning: Bad Token"),
                                            text, type="warning")
                    raise gui.StayOnScreen
                else:                           # all the port data looks good
                    self.firewall.portlist = portlist
        

    def activate_firewall (self, widget):
        if self.sec_none_radio.get_active ():            
            active = not (self.sec_none_radio.get_active())

            self.default_radio.set_sensitive (active)
            self.custom_radio.set_sensitive (active)        
            self.trusted.set_sensitive(active)
            self.incoming.set_sensitive(active)
            self.ports.set_sensitive(active)
            self.label1.set_sensitive(active)
            self.label2.set_sensitive(active)
            self.label3.set_sensitive(active)
        else:
            self.default_radio.set_sensitive (gtk.TRUE)
            self.custom_radio.set_sensitive (gtk.TRUE) 

            if self.custom_radio.get_active ():
                self.trusted.set_sensitive(self.custom_radio.get_active())
                self.incoming.set_sensitive(self.custom_radio.get_active())
                self.ports.set_sensitive(self.custom_radio.get_active())
                self.label1.set_sensitive(self.custom_radio.get_active())
                self.label2.set_sensitive(self.custom_radio.get_active())
                self.label3.set_sensitive(self.custom_radio.get_active())

            else:
                self.trusted.set_sensitive(self.custom_radio.get_active())
                self.incoming.set_sensitive(self.custom_radio.get_active())
                self.ports.set_sensitive(self.custom_radio.get_active())
                self.label1.set_sensitive(self.custom_radio.get_active())
                self.label2.set_sensitive(self.custom_radio.get_active())
                self.label3.set_sensitive(self.custom_radio.get_active())

    def getScreen (self, intf, network, firewall):
	self.firewall = firewall
	self.network = network
        self.intf = intf

        self.devices = self.network.available().keys()
        self.devices.sort()
        
	self.netCBs = {}

        box = gtk.VBox (gtk.FALSE, 5)
        box.set_border_width (5)

        label = gtk.Label (_("Select a security level for the system:  "))
        label.set_alignment (0.0, 0.5)

        label.set_line_wrap (gtk.TRUE)
        
        box.pack_start(label, gtk.FALSE)

        hbox = gtk.HBox (gtk.FALSE)

        self.sec_high_radio = gtk.RadioButton (None, (_("Hi_gh")))
        self.sec_med_radio = gtk.RadioButton (self.sec_high_radio, (_("_Medium")))
        self.sec_none_radio = gtk.RadioButton (self.sec_high_radio, (_("N_o firewall")))
        self.sec_none_radio.connect ("clicked", self.activate_firewall)

        hbox.pack_start (self.sec_high_radio)
        hbox.pack_start (self.sec_med_radio)
        hbox.pack_start (self.sec_none_radio)

        a = gtk.Alignment ()
        a.add (hbox)
        a.set (1.0, 0.5, 0.7, 1.0)

        box.pack_start (a, gtk.FALSE)

        hsep = gtk.HSeparator ()
        box.pack_start (hsep, gtk.FALSE)

        self.default_radio = gtk.RadioButton (None, (_("Use _default firewall rules")))
        self.custom_radio = gtk.RadioButton (self.default_radio, (_("_Customize")))
        self.default_radio.set_active (gtk.TRUE)

        self.default_radio.connect ("clicked", self.activate_firewall)
        self.custom_radio.connect ("clicked", self.activate_firewall)
        
        box.pack_start (self.default_radio, gtk.FALSE)
        box.pack_start (self.custom_radio, gtk.FALSE)

        table = gtk.Table (2, 3)
        box.pack_start (table)

        hbox = gtk.HBox(gtk.FALSE, 10)
        self.label1 = gui.MnemonicLabel (_("_Trusted devices:"))
        self.label1.set_alignment (0.2, 0.0)

        self.trusted = checklist.CheckList(1)
	self.trusted.set_size_request(-1, 80)
        self.label1.set_mnemonic_widget(self.trusted)

        trustedSW = gtk.ScrolledWindow()
        trustedSW.set_border_width(5)
        trustedSW.set_policy(gtk.POLICY_NEVER, gtk.POLICY_AUTOMATIC)
        trustedSW.set_shadow_type(gtk.SHADOW_IN)
        trustedSW.add(self.trusted)

        if self.devices != []:
            table.attach (self.label1, 0, 1, 0, 1, gtk.FILL, gtk.FILL, 5, 5)
            table.attach (trustedSW, 1, 2, 0, 1, gtk.EXPAND|gtk.FILL, gtk.FILL, 5, 5)

            for device in self.devices:
                if device in self.firewall.trustdevs:
                    self.trusted.append_row ((device, device), gtk.TRUE)
                else:
                    self.trusted.append_row ((device, device), gtk.FALSE)
                if self.network.netdevices[device].get('bootproto') == 'dhcp':
                    self.firewall.dhcp = 1

        hbox = gtk.HBox(gtk.FALSE, 10)        
        self.label2 = gui.MnemonicLabel (_("_Allow incoming:"))
        self.label2.set_alignment (0.2, 0.0)
        self.incoming = checklist.CheckList(1)
	self.incoming.set_size_request(-1, 140)
        self.label2.set_mnemonic_widget(self.incoming)

        incomingSW = gtk.ScrolledWindow()
        incomingSW.set_border_width(5)
        incomingSW.set_policy(gtk.POLICY_NEVER, gtk.POLICY_AUTOMATIC)
        incomingSW.set_shadow_type(gtk.SHADOW_IN)
        incomingSW.add(self.incoming)
        
        table.attach (self.label2, 0, 1, 1, 2, gtk.FILL, gtk.FILL, 5, 5)
        table.attach (incomingSW, 1, 2, 1, 2, gtk.EXPAND|gtk.FILL, gtk.FILL, 5, 5)

        self.knownPorts = {"DHCP": self.firewall.dhcp,
                           "SSH": self.firewall.ssh,
                           "Telnet": self.firewall.telnet,
                           "WWW (HTTP)": self.firewall.http,
                           "Mail (SMTP)": self.firewall.smtp,
                           "FTP": self.firewall.ftp}

        for item in self.knownPorts.keys():
            self.incoming.append_row ((item, ""), self.knownPorts[item])

        self.label3 = gui.MnemonicLabel (_("Other _ports:"))
        self.ports = gtk.Entry ()
        self.label3.set_mnemonic_widget(self.ports)

        table.attach (self.label3, 0, 1, 2, 3, gtk.FILL, gtk.FILL, 5, 5)
        table.attach (self.ports, 1, 2, 2, 3, gtk.EXPAND|gtk.FILL, gtk.FILL, 10, 5)

        if self.firewall.enabled == 0:
            self.sec_none_radio.set_active (gtk.TRUE)
        elif self.firewall.policy == 0:
            self.sec_high_radio.set_active (gtk.TRUE)
        elif self.firewall.policy == 1:
            self.sec_med_radio.set_active (gtk.TRUE)

        if self.firewall.portlist != "":
            self.ports.set_text (self.firewall.portlist)

        if self.firewall.custom == 1:
            self.custom_radio.set_active(gtk.TRUE)
        else:
            self.trusted.set_sensitive(gtk.FALSE)
            self.incoming.set_sensitive(gtk.FALSE)
            self.ports.set_sensitive(gtk.FALSE)
            self.label1.set_sensitive(gtk.FALSE)
            self.label2.set_sensitive(gtk.FALSE)
            self.label3.set_sensitive(gtk.FALSE)

        return box


