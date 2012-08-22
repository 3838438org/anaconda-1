#
# Copyright (C) 2012  Red Hat, Inc.
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
# Red Hat Author(s): Vratislav Podzimek <vpodzime@redhat.com>
#

"""
Module providing functions for getting the list of timezones, writing timezone
configuration, valid timezones recognition etc.

"""

import os
import pytz
from collections import OrderedDict

from pyanaconda import localization

import logging
log = logging.getLogger("anaconda")

class TimezoneConfigError(Exception):
    """Exception class for timezone configuration related problems"""
    pass

def write_timezone_config(timezone, root):
    """
    Write timezone configuration for the system specified by root.

    @param timezone: ksdata.timezone object
    @param root: path to the root
    @raise: TimezoneConfigError

    """

    # we want to create a relative symlink
    tz_file = "/usr/share/zoneinfo/" + timezone.timezone
    rooted_tz_file = os.path.normpath(root + tz_file)
    relative_path = os.path.normpath("../" + tz_file)
    link_path = os.path.normpath(root + "/etc/localtime")

    if not os.access(rooted_tz_file, os.R_OK):
        log.error("Timezone to be linked (%s) doesn't exist" % rooted_tz_file)
    else:
        try:
            os.symlink(relative_path, link_path)
        except OSError as oserr:
            log.error("Error when symlinking timezone (from %s): %s" % \
                      (rooted_tz_file, oserr.strerror))

    try:
        with open(os.path.normpath(root + "/etc/sysconfig/clock"), "w") as fobj:
            fobj.write('ZONE="%s"\n' % timezone.timezone)
    except IOError as ioerr:
        msg = "Error while writing /etc/sysconfig/clock file: %s" % \
                ioerr.strerror
        raise TimezoneConfigError(msg)

    try:
        fobj = open(os.path.normpath(root + "/etc/adjtime"), "r")
        lines = fobj.readlines()
        fobj.close()
    except IOError:
        lines = [ "0.0 0 0.0\n", "0\n" ]

    try:
        with open(os.path.normpath(root + "/etc/adjtime"), "w") as fobj:
            fobj.write(lines[0])
            fobj.write(lines[1])
            if timezone.isUtc:
                fobj.write("UTC\n")
            else:
                fobj.write("LOCAL\n")
    except IOError as ioerr:
        msg = "Error while writing /etc/adjtime file: %s" % ioerr.strerror
        raise TimezoneConfigError(msg)

def get_all_territory_timezones(territory):
    """
    Return the list of timezones for a given territory.

    @param territory: either localization.LocaleInfo or territory

    """

    if isinstance(territory, localization.LocaleInfo):
        territory = territory.territory

    try:
        timezones = pytz.country_timezones(territory)
    except KeyError:
        timezones = list()

    return timezones


def get_preferred_timezone(territory):
    """
    Get the preferred timezone for a given territory. Note that this function
    simply returns the first timezone in the list of timezones for a given
    territory.

    @param territory: either localization.LocaleInfo or territory

    """

    try:
        timezone = get_all_territory_timezones(territory)[0]
    except IndexError:
        timezone = None

    return timezone

def get_all_regions_and_timezones():
    """
    Get a dictionary mapping the regions to the list of their timezones.

    @rtype: dict

    """

    result = OrderedDict()

    for tz in pytz.common_timezones:
        parts = tz.split("/", 1)

        if len(parts) > 1:
            if parts[0] not in result:
                result[parts[0]] = set()
            result[parts[0]].add(parts[1])

    return result

def is_valid_timezone(timezone):
    """
    Check if a given string is an existing timezone.

    @type timezone: str
    @rtype: bool

    """

    return timezone in pytz.common_timezones

