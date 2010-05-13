#
# network.py - network configuration install data
#
# Copyright (C) 2001, 2002, 2003, 2004, 2005, 2006, 2007  Red Hat, Inc.
#               2008, 2009
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
# Author(s): Matt Wilson <ewt@redhat.com>
#            Erik Troan <ewt@redhat.com>
#            Mike Fulbright <msf@redhat.com>
#            Brent Fox <bfox@redhat.com>
#            David Cantrell <dcantrell@redhat.com>
#

import string
import shutil
import isys
import iutil
import socket
import struct
import os
import time
import dbus
from flags import flags
from simpleconfig import IfcfgFile

import gettext
_ = lambda x: gettext.ldgettext("anaconda", x)

import logging
log = logging.getLogger("anaconda")

sysconfigDir = "/etc/sysconfig"
netscriptsDir = "%s/network-scripts" % (sysconfigDir)
networkConfFile = "%s/network" % (sysconfigDir)
ifcfgLogFile = "/tmp/ifcfg.log"
CONNECTION_TIMEOUT = 45

class IPError(Exception):
    pass

class IPMissing(Exception):
    pass

def sanityCheckHostname(hostname):
    if len(hostname) < 1:
        return None

    if len(hostname) > 255:
        return _("Hostname must be 255 or fewer characters in length.")

    validStart = string.ascii_letters + string.digits
    validAll = validStart + ".-"

    if string.find(validStart, hostname[0]) == -1:
        return _("Hostname must start with a valid character in the ranges "
                 "'a-z', 'A-Z', or '0-9'")

    for i in range(1, len(hostname)):
        if string.find(validAll, hostname[i]) == -1:
            return _("Hostnames can only contain the characters 'a-z', 'A-Z', '0-9', '-', or '.'")

    return None

# Try to determine what the hostname should be for this system
def getDefaultHostname(anaconda):
    isys.resetResolv()

    hn = None
    bus = dbus.SystemBus()
    nm = bus.get_object(isys.NM_SERVICE, isys.NM_MANAGER_PATH)
    nm_props_iface = dbus.Interface(nm, isys.DBUS_PROPS_IFACE)

    active_connections = nm_props_iface.Get(isys.NM_MANAGER_IFACE, "ActiveConnections")

    # XXX: account for Ip6Config objects when NetworkManager supports them
    for connection in active_connections:
        active_connection = bus.get_object(isys.NM_SERVICE, connection)
        active_connection_props_iface = dbus.Interface(active_connection, isys.DBUS_PROPS_IFACE)
        devices = active_connection_props_iface.Get(isys.NM_MANAGER_IFACE, 'Devices')

        for device_path in devices:
            device = bus.get_object(isys.NM_SERVICE, device_path)
            device_props_iface = dbus.Interface(device, isys.DBUS_PROPS_IFACE)

            ip4_config_path = device_props_iface.Get(isys.NM_MANAGER_IFACE, 'Ip4Config')
            ip4_config_obj = bus.get_object(isys.NM_SERVICE, ip4_config_path)
            ip4_config_props = dbus.Interface(ip4_config_obj, isys.DBUS_PROPS_IFACE)

            # addresses (3-element list:  ipaddr, netmask, gateway)
            addrs = ip4_config_props.Get(isys.NM_MANAGER_IFACE, "Addresses")[0]
            try:
                tmp = struct.pack('I', addrs[0])
                ipaddr = socket.inet_ntop(socket.AF_INET, tmp)
                hinfo = socket.gethostbyaddr(ipaddr)

                if len(hinfo) == 3:
                    hn = hinfo[0]
                else:
                    continue
            except:
                continue

    if hn and hn != 'localhost' and hn != 'localhost.localdomain':
        return hn

    try:
        hn = anaconda.id.network.hostname
    except:
        hn = None

    if not hn or hn == '(none)' or hn == 'localhost' or hn == 'localhost.localdomain':
        hn = socket.gethostname()

    if not hn or hn == '(none)' or hn == 'localhost':
        hn = 'localhost.localdomain'

    return hn

# return if the device is of a type that requires a ptpaddr to be specified
def isPtpDev(devname):
    if devname.startswith("ctc"):
        return True
    return False

def _anyUsing(method):
    # method names that NetworkManager might use
    if method == 'auto':
        methods = (method, 'dhcp')
    else:
        methods = (method)

    try:
        bus = dbus.SystemBus()
        nm = bus.get_object(isys.NM_SERVICE, isys.NM_MANAGER_PATH)
        nm_props_iface = dbus.Interface(nm, isys.DBUS_PROPS_IFACE)
        active_connections = nm_props_iface.Get(isys.NM_MANAGER_IFACE, "ActiveConnections")

        for path in active_connections:
            active = bus.get_object(isys.NM_SERVICE, path)
            active_props_iface = dbus.Interface(active, isys.DBUS_PROPS_IFACE)

            active_service_name = active_props_iface.Get(isys.NM_ACTIVE_CONNECTION_IFACE, "ServiceName")
            active_path = active_props_iface.Get(isys.NM_ACTIVE_CONNECTION_IFACE, "Connection")

            connection = bus.get_object(active_service_name, active_path)
            connection_iface = dbus.Interface(connection, isys.NM_CONNECTION_IFACE)
            settings = connection_iface.GetSettings()

            # XXX: add support for Ip6Config when it appears
            ip4_setting = settings['ipv4']
            if not ip4_setting or not ip4_setting['method'] or ip4_setting['method'] in methods:
                return True

            return False
    except:
        return False

# determine whether any active at boot devices are using dhcp or dhcpv6
def anyUsingDHCP():
    return _anyUsing('auto')

# determine whether any active at boot devices are using static IP config
def anyUsingStatic():
    return _anyUsing('manual')

# sanity check an IP string.
def sanityCheckIPString(ip_string):
    if ip_string.strip() == "":
        raise IPMissing, _("IP address is missing.")

    if ip_string.find(':') == -1 and ip_string.find('.') > 0:
        family = socket.AF_INET
        errstr = _("IPv4 addresses must contain four numbers between 0 and 255, separated by periods.")
    elif ip_string.find(':') > 0 and ip_string.find('.') == -1:
        family = socket.AF_INET6
        errstr = _("'%s' is not a valid IPv6 address.") % ip_string
    else:
        raise IPError, _("'%s' is an invalid IP address.") % ip_string

    try:
        socket.inet_pton(family, ip_string)
    except socket.error:
        raise IPError, errstr

def hasActiveNetDev():
    try:
        bus = dbus.SystemBus()
        nm = bus.get_object(isys.NM_SERVICE, isys.NM_MANAGER_PATH)
        props = dbus.Interface(nm, isys.DBUS_PROPS_IFACE)
        state = props.Get(isys.NM_SERVICE, "State")

        if int(state) == isys.NM_STATE_CONNECTED:
            return True
        else:
            return False
    except:
        return False

# Return a list of device names (e.g., eth0) for all active devices.
# Returning a list here even though we will almost always have one
# device.  NM uses lists throughout its D-Bus communication, so trying
# to follow suit here.  Also, if this uses a list now, we can think
# about multihomed hosts during installation later.
def getActiveNetDevs():
    active_devs = set()

    bus = dbus.SystemBus()
    nm = bus.get_object(isys.NM_SERVICE, isys.NM_MANAGER_PATH)
    nm_props_iface = dbus.Interface(nm, isys.DBUS_PROPS_IFACE)

    active_connections = nm_props_iface.Get(isys.NM_MANAGER_IFACE, "ActiveConnections")

    for connection in active_connections:
        active_connection = bus.get_object(isys.NM_SERVICE, connection)
        active_connection_props_iface = dbus.Interface(active_connection, isys.DBUS_PROPS_IFACE)
        devices = active_connection_props_iface.Get(isys.NM_MANAGER_IFACE, 'Devices')

        for device_path in devices:
            device = bus.get_object(isys.NM_SERVICE, device_path)
            device_props_iface = dbus.Interface(device, isys.DBUS_PROPS_IFACE)

            interface_name = device_props_iface.Get(isys.NM_MANAGER_IFACE, 'Interface')
            active_devs.add(interface_name)

    ret = list(active_devs)
    ret.sort()
    return ret

def logIfcfgFile(path, header="\n"):
    logfile = ifcfgLogFile
    if not os.access(path, os.R_OK):
        return
    f = open(path, 'r')
    lf = open(logfile, 'a')
    lf.write(header)
    lf.write(f.read())
    lf.close()
    f.close()

def logIfcfgFiles(header="\n"):

    lf = open(ifcfgLogFile, 'a')
    lf.write(header)
    lf.close()

    devprops = isys.getDeviceProperties(dev=None)
    for device in devprops:
        path = "%s/ifcfg-%s" % (netscriptsDir, device)
        logIfcfgFile(path, "===== %s\n" % (path,))

class NetworkDevice(IfcfgFile):

    def __init__(self, dir, iface, logfile='/tmp/ifcfg.log'):
        IfcfgFile.__init__(self, dir, iface)
        self.logfile = logfile
        if iface.startswith('ctc'):
            self.info["TYPE"] = "CTC"
        self.description = ""

    def clear(self):
        IfcfgFile.clear(self)
        if self.iface.startswith('ctc'):
            self.info["TYPE"] = "CTC"

    def __str__(self):
        s = ""
        keys = self.info.keys()
        keys.sort()
        keys.remove("DEVICE")
        keys.insert(0, "DEVICE")
        if "KEY" in keys:
            keys.remove("KEY")
        if iutil.isS390() and ("HWADDR" in keys):
            keys.remove("HWADDR")
        # make sure we include autoneg in the ethtool line
        if 'ETHTOOL_OPTS' in keys:
            eopts = self.get('ETHTOOL_OPTS')
            if "autoneg" not in eopts:
                self.set(('ETHTOOL_OPTS', "autoneg off %s" % eopts))

        for key in keys:
            if self.info[key] is not None:
                s = s + key + '="' + self.info[key] + '"\n'

        return s

    def loadIfcfgFile(self):
        self.clear()
        IfcfgFile.read(self)

    def writeIfcfgFile(self, dir=None):
        IfcfgFile.write(self, dir)

    def log(self, header="\n"):
        lf = open(self.logfile, 'a')
        lf.write(header)
        lf.close()
        self.log_file()
        self.log_write_file()
        self.log_values()

    def log_values(self, header="\n"):
        lf = open(self.logfile, 'a')
        lf.write(header)
        lf.write("== values for file %s\n" % self.path)
        lf.write(IfcfgFile.__str__(self))
        lf.close()

    def log_write_file(self, header="\n"):
        lf = open(self.logfile, 'a')
        lf.write(header)
        lf.write("== file to be written for %s\n" % self.path)
        lf.write(self.__str__())
        lf.close()

    def log_file(self, header="\n"):
        f = open(self.path, 'r')
        lf = open(self.logfile, 'a')
        lf.write(header)
        lf.write("== file %s\n" % self.path)
        lf.write(f.read())
        lf.close()
        f.close()


class Network:

    def __init__(self):

        self.hostname = socket.gethostname()
        self.overrideDHCPhostname = False
        self.update()

    def update(self):

        self.netdevices = {}
        self.ksdevice = None
        self.domains = []

        # populate self.netdevices
        devhash = isys.getDeviceProperties(dev=None)
        for iface in devhash.keys():
            device = NetworkDevice(netscriptsDir, iface, logfile=ifcfgLogFile)
            device.loadIfcfgFile()
            device.log("===== Network.update\n")

            if device.get('DOMAIN'):
                self.domains.append(device.get('DOMAIN'))
            # TODORV - the last iface in loop wins, might be ok,
            #          not worthy of special juggling
            if device.get('HOSTNAME'):
                self.hostname = device.get('HOSTNAME')

            device.description = isys.getNetDevDesc(iface)

            self.netdevices[iface] = device


        ksdevice = flags.cmdline.get('ksdevice', None)
        if ksdevice:
            for dev in self.netdevices:
                if ksdevice == 'link' and isys.getLinkStatus(dev):
                    self.ksdevice = dev
                    break
                elif ksdevice == dev:
                    self.ksdevice = dev
                    break
                elif ':' in ksdevice:
                    if ksdevice.upper() == self.netdevices[dev].get('HWADDR'):
                        self.ksdevice = dev
                        break



    def getDevice(self, device):
        return self.netdevices[device]

    def getKSDevice(self):
        if self.ksdevice is None:
            return None

        try:
            return self.netdevices[self.ksdevice]
        except:
            return None

    def setHostname(self, hn):
        self.hostname = hn

    def setDNS(self, ns, device):
        dns = ns.split(',')
        i = 1
        for addr in dns:
            addr = addr.strip()
            dnslabel = "DNS%d" % (i,)
            self.netdevices[device].set((dnslabel, addr))
            i += 1

    def setGateway(self, gw, device):
        self.netdevices[device].set(('GATEWAY', gw))

    def lookupHostname(self):
        # can't look things up if they don't exist!
        if not self.hostname or self.hostname == "localhost.localdomain":
            return None

        if not hasActiveNetDev():
            log.warning("no network devices were available to look up host name")
            return None

        try:
            (family, socktype, proto, canonname, sockaddr) = \
                socket.getaddrinfo(self.hostname, None, socket.AF_INET)[0]
            (ip, port) = sockaddr
        except:
            try:
                (family, socktype, proto, canonname, sockaddr) = \
                    socket.getaddrinfo(self.hostname, None, socket.AF_INET6)[0]
                (ip, port, flowinfo, scopeid) = sockaddr
            except:
                return None

        return ip

    # devices == None => set for all
    def setNMControlledDevices(self, devices=None):
        for devname, device in self.netdevices.items():
            if devices and devname not in devices:
                device.set(('NM_CONTROLLED', 'no'))
            else:
                device.set(('NM_CONTROLLED', 'yes'))
            device.writeIfcfgFile()
            device.log_file("device set to be nm controlled\n")

    # devices == None => set for all
    def updateActiveDevices(self, devices=None):
        for devname, device in self.netdevices.items():
            if devices and devname not in devices:
                device.set(('ONBOOT', 'no'))
            else:
                device.set(('ONBOOT', 'yes'))
            device.writeIfcfgFile()
            device.log_file("updateActiveDevices\n")

    def getOnbootIfaces(self):
        ifaces = []
        for iface, device in self.netdevices.items():
            if device.get('ONBOOT') == "yes":
                ifaces.append(iface)
        return ifaces

    def writeKS(self, f):
        devNames = self.netdevices.keys()
        devNames.sort()

        if len(devNames) == 0:
            return

        for devName in devNames:
            dev = self.netdevices[devName]

            if dev.get('bootproto').lower() == 'dhcp' or dev.get('ipaddr'):
                f.write("network --device %s" % dev.get('device'))

                if dev.get('MTU') and dev.get('MTU') != 0:
                    f.write(" --mtu=%s" % dev.get('MTU'))

                onboot = dev.get("onboot")
                if onboot and onboot == "no":
                    f.write(" --onboot no")
                if dev.get('bootproto').lower() == 'dhcp':
                    f.write(" --bootproto dhcp")
                    if dev.get('dhcpclass'):
                        f.write(" --dhcpclass %s" % dev.get('dhcpclass'))
                    if self.overrideDHCPhostname:
                        if (self.hostname and
                            self.hostname != "localhost.localdomain"):
                            f.write(" --hostname %s" % self.hostname)
                else:
                    f.write(" --bootproto static --ip %s" % dev.get('ipaddr'))

                    if dev.get('netmask'):
                        f.write(" --netmask %s" % dev.get('netmask'))

                    if dev.get('GATEWAY'):
                        f.write(" --gateway %s" % (dev.get('GATEWAY'),))

                    dnsline = ''
                    for key in dev.info.keys():
                        if key.upper().startswith('DNS'):
                            if dnsline == '':
                                dnsline = dev.get(key)
                            else:
                                dnsline += "," + dev.get(key)

                    if dnsline != '':
                        f.write(" --nameserver %s" % (dnsline,))

                    if (self.hostname and
                        self.hostname != "localhost.localdomain"):
                        f.write(" --hostname %s" % self.hostname)

                f.write("\n")

    def hasNameServers(self, hash):
        if hash.keys() == []:
            return False

        for key in hash.keys():
            if key.upper().startswith('DNS'):
                return True

        return False

    def write(self, instPath='', anaconda=None):

        devices = self.netdevices.values()

        if len(devices) == 0:
            return

        sysconfig = instPath + sysconfigDir
        netscripts = instPath + netscriptsDir
        destnetwork = instPath + networkConfFile

        if not os.path.isdir(netscripts):
            iutil.mkdirChain(netscripts)

        # /etc/sysconfig/network-scripts/ifcfg-*
        for dev in devices:
            device = dev.get('DEVICE')

            cfgfile = "%s/ifcfg-%s" % (netscripts, device,)
            if (instPath) and (os.path.isfile(cfgfile)):
                continue

            bootproto = dev.get('BOOTPROTO').lower()
            # write out the hostname as DHCP_HOSTNAME if given (#81613)
            if (bootproto == 'dhcp' and self.hostname and
                self.overrideDHCPhostname):
                dev.set(('DHCP_HOSTNAME', self.hostname))

            # tell NetworkManager not to touch any interfaces used during
            # installation when / is on a network backed device.
            if anaconda is not None:
                import storage
                rootdev = anaconda.id.storage.rootDevice
                # FIXME: use d.host_address to only add "NM_CONTROLLED=no"
                # for interfaces actually used enroute to the device
                for d in anaconda.id.storage.devices:
                    if isinstance(d, storage.devices.NetworkStorageDevice) and\
                       (rootdev.dependsOn(d) or d.nic == device):
                        dev.set(('NM_CONTROLLED', 'no'))
                        break

            dev.writeIfcfgFile(netscripts)
            dev.log_file("===== write\n")

            # XXX: is this necessary with NetworkManager?
            # handle the keys* files if we have those
            if dev.get("KEY"):
                cfgfile = "%s/keys-%s" % (netscripts, device,)
                if not instPath == '' and os.path.isfile(cfgfile):
                    continue

                newkey = "%s/keys-%s.new" % (netscripts, device,)
                f = open(newkey, "w")
                f.write("KEY=%s\n" % (dev.get('KEY'),))
                f.close()
                os.chmod(newkey, 0600)

                destkey = "%s/keys-%s" % (netscripts, device,)
                shutil.move(newkey, destkey)

            # /etc/dhclient-DEVICE.conf
            dhclientconf = '/etc/dhclient-' + device + '.conf'
            if os.path.isfile(dhclientconf):
                destdhclientconf = '%s%s' % (instPath, dhclientconf,)
                try:
                    shutil.copy(dhclientconf, destdhclientconf)
                except:
                    log.warning("unable to copy %s to target system" % (dhclientconf,))

        # /etc/sysconfig/network
        if (not instPath) or (not os.path.isfile(destnetwork)) or flags.livecdInstall:
            newnetwork = "%s.new" % (destnetwork,)

            f = open(newnetwork, "w")
            f.write("NETWORKING=yes\n")
            f.write("HOSTNAME=")

            # use instclass hostname if set(kickstart) to override
            if self.hostname:
                f.write(self.hostname + "\n")
            else:
                f.write("localhost.localdomain\n")

            if dev.get('GATEWAY'):
                f.write("GATEWAY=%s\n" % (dev.get('GATEWAY'),))

            if dev.get('IPV6_DEFAULTGW'):
                f.write("IPV6_DEFAULTGW=%s\n" % (dev.get('IPV6_DEFAULTGW'),))

            f.close()
            shutil.move(newnetwork, destnetwork)

        # If the hostname was not looked up, but typed in by the user,
        # domain might not be computed, so do it now.
        domainname = None
        if "." in self.hostname:
            fqdn = self.hostname
        else:
            fqdn = socket.getfqdn(self.hostname)

        if fqdn in [ "localhost.localdomain", "localhost",
                     "localhost6.localdomain6", "localhost6",
                     self.hostname ] or "." not in fqdn:
            fqdn = None

        if fqdn:
            domainname = fqdn.split('.', 1)[1]
            if domainname in [ "localdomain", "localdomain6" ]:
                domainname = None
        else:
            domainname = None

        if self.domains == ["localdomain"] or not self.domains:
            if domainname:
                self.domains = [domainname]

        # /etc/resolv.conf
        if (not instPath) or (not os.path.isfile(instPath + '/etc/resolv.conf')) or flags.livecdInstall:
            if os.path.isfile('/etc/resolv.conf') and instPath != '':
                destresolv = "%s/etc/resolv.conf" % (instPath,)
                shutil.copy('/etc/resolv.conf', destresolv)
            elif (self.domains != ['localdomain'] and self.domains) or \
                self.hasNameServers(dev.info):
                resolv = "%s/etc/resolv.conf" % (instPath,)

                f = open(resolv, "w")

                if self.domains != ['localdomain'] and self.domains:
                    f.write("search %s\n" % (string.joinfields(self.domains, ' '),))

                for key in dev.info.keys():
                    if key.upper().startswith('DNS'):
                        f.write("nameserver %s\n" % (dev.get(key),))

                f.close()

        # /etc/udev/rules.d/70-persistent-net.rules
        rules = "/etc/udev/rules.d/70-persistent-net.rules"
        destRules = instPath + rules
        if (not instPath) or (not os.path.isfile(destRules)) or \
           flags.livecdInstall:
            if not os.path.isdir("%s/etc/udev/rules.d" %(instPath,)):
                iutil.mkdirChain("%s/etc/udev/rules.d" %(instPath,))

            if os.path.isfile(rules) and rules != destRules:
                shutil.copy(rules, destRules)
            else:
                f = open(destRules, "w")
                f.write("""
# This file was automatically generated by the /lib/udev/write_net_rules
# program run by the persistent-net-generator.rules rules file.
#
# You can modify it, as long as you keep each rule on a single line.

""")
                for dev in self.netdevices.values():
                    addr = dev.get("HWADDR")
                    if not addr:
                        continue
                    devname = dev.get("DEVICE")
                    basename = devname
                    while basename != "" and basename[-1] in string.digits:
                        basename = basename[:-1]

                    # rules are case senstive for address. Lame.
                    addr = addr.lower()

                    s = ""
                    if len(dev.description) > 0:
                        s = "# %s (rule written by anaconda)\n" % (dev.description,)
                    else:
                        s = "# %s (rule written by anaconda)\n" % (devname,)
                    s = s + 'SUBSYSTEM==\"net\", ACTION==\"add\", DRIVERS=="?*", ATTR{address}=="%s", ATTR{type}=="1", KERNEL=="%s*", NAME="%s"\n' % (addr, basename, devname,)

                    f.write(s)

                f.close()

    def waitForDevicesActivation(self, devices):
        waited_devs_props = {}

        bus = dbus.SystemBus()
        nm = bus.get_object(isys.NM_SERVICE, isys.NM_MANAGER_PATH)
        device_paths = nm.get_dbus_method("GetDevices")()
        for device_path in device_paths:
            device = bus.get_object(isys.NM_SERVICE, device_path)
            device_props_iface = dbus.Interface(device, isys.DBUS_PROPS_IFACE)
            iface = str(device_props_iface.Get(isys.NM_MANAGER_IFACE, "Interface"))
            if iface in devices:
                waited_devs_props[iface] = device_props_iface

        i = 0
        while True:
            for dev, device_props_iface in waited_devs_props.items():
                state = device_props_iface.Get(isys.NM_MANAGER_IFACE, "State")
                if state == isys.NM_DEVICE_STATE_ACTIVATED:
                    waited_devs_props.pop(dev)
            if len(waited_devs_props) == 0:
                return []
            if i >= CONNECTION_TIMEOUT:
                return waited_devs_props.keys()
            i += 1
            time.sleep(1)

    # write out current configuration state and wait for NetworkManager
    # to bring the device up, watch NM state and return to the caller
    # once we have a state
    def waitForConnection(self):
        bus = dbus.SystemBus()
        nm = bus.get_object(isys.NM_SERVICE, isys.NM_MANAGER_PATH)
        props = dbus.Interface(nm, isys.DBUS_PROPS_IFACE)

        i = 0
        while i < CONNECTION_TIMEOUT:
            state = props.Get(isys.NM_SERVICE, "State")
            if int(state) == isys.NM_STATE_CONNECTED:
                isys.resetResolv()
                return True
            i += 1
            time.sleep(1)

        state = props.Get(isys.NM_SERVICE, "State")
        if int(state) == isys.NM_STATE_CONNECTED:
            isys.resetResolv()
            return True

        return False

    # write out current configuration state and wait for NetworkManager
    # to bring the device up, watch NM state and return to the caller
    # once we have a state
    def bringUp(self):
        self.write()
        return self.waitForConnection()

    # get a kernel cmdline string for dracut needed for access to host host
    def dracutSetupString(self, networkStorageDevice):
        netargs=""

        if networkStorageDevice.nic:
            # Storage bound to a specific nic (ie FCoE)
            nic = networkStorageDevice.nic
        else:
            # Storage bound through ip, find out which interface leads to host
            host = networkStorageDevice.host_address
            route = iutil.execWithCapture("ip", [ "route", "get", "to", host ])
            if not route:
                log.error("Could net get interface for route to %s" % host)
                return ""

            routeInfo = route.split()
            if routeInfo[0] != host or len(routeInfo) < 5 or \
               "dev" not in routeInfo or routeInfo.index("dev") > 3:
                log.error('Unexpected "ip route get to %s" reply: %s' %
                          (host, routeInfo))
                return ""

            nic = routeInfo[routeInfo.index("dev") + 1]

        if nic not in self.netdevices.keys():
            log.error('Unknown network interface: %s' % nic)
            return ""

        dev = self.netdevices[nic]

        if networkStorageDevice.host_address:
            if dev.get('bootproto').lower() == 'dhcp':
                netargs += "ip=%s:dhcp" % nic
            else:
                if dev.get('GATEWAY'):
                    gateway = dev.get('GATEWAY')
                else:
                    gateway = ""

                if self.hostname:
                    hostname = self.hostname
                else:
                    hostname = ""

                netargs += "ip=%s::%s:%s:%s:%s:none" % (dev.get('ipaddr'),
                           gateway, dev.get('netmask'), hostname, nic)

        hwaddr = dev.get("HWADDR")
        if hwaddr:
            if netargs != "":
                netargs += " "

            netargs += "ifname=%s:%s" % (nic, hwaddr.lower())

        nettype = dev.get("NETTYPE")
        subchannels = dev.get("SUBCHANNELS")
        if iutil.isS390() and nettype and subchannels:
            if netargs != "":
                netargs += " "

            netargs += "rd_CCW=%s,%s" % (nettype, subchannels)

            options = dev.get("OPTIONS").strip("'\"")
            if options:
                options = filter(lambda x: x != '', options.split(' '))
                netargs += ",%s" % (','.join(options))

        return netargs
