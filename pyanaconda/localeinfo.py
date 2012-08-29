# __init__.py
# Locale info used by getlangnames.py and languages.py.
#
# Copyright (C) 2011  Red Hat, Inc.
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


""" Basic locale operations useful during both Anaconda build time and run time.

    This module can be imported without importing pyanaconda/__init__.py and it
    is desirable to keep it that way.
"""

import os
import string

def get(default):
    localeInfo = {}
    # nick -> (name, short name, text mode supported, keyboard, timezone) mapping
    search = ('lang-table', '/tmp/updates/lang-table', '/etc/lang-table',
              '/usr/share/anaconda/lang-table')
    for path in search:
        if os.access(path, os.R_OK):
            f = open(path, "r")
            for line in f.readlines():
                string.strip(line)
                l = string.split(line, '\t')

                # throw out invalid lines
                if len(l) < 6:
                    continue

                ts = l[2] == "True"

                localeInfo[l[3]] = (l[0], l[1], ts, l[4], string.strip(l[5]))

            f.close()
            break

    # Hard code this to prevent errors in the build environment.
    localeInfo['C'] = localeInfo[default]
    return localeInfo
