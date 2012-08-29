#
# language.py: install data component that stores information about both
#              installer runtime language choice and installed system
#              language support.
#
# Copyright (C) 2001, 2002, 2003, 2004, 2005, 2009  Red Hat, Inc.
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

import os
import string
import locale

import gettext
from pyanaconda.constants import ROOT_PATH, DEFAULT_LANG
import localization
from simpleconfig import SimpleConfigFile

import logging
log = logging.getLogger("anaconda")

class Language(object):
    def _setInstLang(self, value):
        # Always store in its full form so we know what we're comparing with.
        try:
            self._instLang = self._canonLang(value)
        except ValueError:
            # If the language isn't listed in lang-table, we won't know what
            # keyboard/etc. to use.  However, we can still set the $LANG
            # to that and make sure it works in the installed system.
            self._instLang = value

        # If we're running in text mode, value may not be a supported language
        # to display.  Fall back to the default for now.
        if self.displayMode == 't':
            for (lang, info) in self.localeInfo.iteritems():
                if lang == self._instLang and info[2] == "False":
                    self._instLang = self._default
                    break

        # Now set some things to make sure the language setting takes effect
        # right now.
        os.environ["LANG"] = self._instLang
        os.environ["LC_NUMERIC"] = "C"

        try:
            locale.setlocale(locale.LC_ALL, "")
        except locale.Error:
            pass

        # XXX: oh ick.  this is the sort of thing which you should never do...
        # but we switch languages at runtime and thus need to invalidate
        # the set of languages/mofiles which gettext knows about
        gettext._translations = {}

    def _getInstLang(self):
        # If we were given a language that's not in lang-table, lie and say
        # we're using the default.  This prevents us from having to check all
        # over the place.
        if self._instLang in self.localeInfo.keys():
            return self._instLang
        else:
            return self._default

    # The language being displayed while anaconda is running.
    instLang = property(lambda s: s._getInstLang(), lambda s, v: s._setInstLang(v))

    def _setSystemLang(self, value):
        # Always store in its full form so we know what we're comparing with.
        try:
            self._systemLang = self._canonLang(value)
        except ValueError:
            # If the language isn't listed in lang-table, we won't know what
            # keyboard/etc. to use.  However, we can still set the $LANG
            # to that and make sure it works in the installed system.
            self._systemLang = value

        # Now set a bunch of other things that'll get written to
        # /etc/sysconfig/i18n on the installed system.
        self.info["LANG"] = self._systemLang

        if not self.localeInfo.has_key(self._systemLang):
            return

        if self.localeInfo[self._systemLang][2] == "False":
            self.info["SYSFONT"] = None
        else:
            self.info["SYSFONT"] = "latarcyrheb-sun16"

        # XXX hack - because of exceptional cases on the var - zh_CN.GB2312
        if self._systemLang == "zh_CN.GB18030":
            self.info["LANGUAGE"] = "zh_CN.GB18030:zh_CN.GB2312:zh_CN"

    # The language to use on the installed system.  This can differ from the
    # language being used during anaconda.  For instance, text installs cannot
    # display all languages (CJK, Indic, etc.).
    systemLang = property(lambda s: s._systemLang, lambda s, v: s._setSystemLang(v))

    def __init__ (self, display_mode = 'g'):
        self._default = DEFAULT_LANG
        self.displayMode = display_mode
        self.info = {}
        self.nativeLangNames = {}

        # English name -> native name mapping
        search = ('lang-names', '/usr/share/anaconda/lang-names')
        for path in search:
            if os.access(path, os.R_OK):
                f = open(path, 'r')
                for line in f.readlines():
                    lang, native = string.split(line, '\t')
                    native = native.strip()
                    self.nativeLangNames[lang] = native

                f.close()
                break

        self.localeInfo = localeinfo.get(self._default)

        # instLang must be set after localeInfo is populated, in case the
        # current setting is unsupported by anaconda..
        self.instLang = os.environ.get("LANG", self._default)
        self.systemLang = os.environ.get("LANG", self._default)

    def _canonLang(self, lang):
        """Convert the shortened form of a language name into the full
           version.  If it's not found, raise ValueError.

           Example:  fr    -> fr_FR.UTF-8
                     fr_FR -> fr_FR.UTF-8
                     fr_CA -> ValueError
        """
        for key in self.localeInfo.keys():
            if lang in localization.expand_langs(key):
                return key

        raise ValueError

    def available(self):
        return self.nativeLangNames.keys()

    def dracutSetupArgs(self):
        args=set()

        for (key, val) in self.info.iteritems():
            if val != None:
                args.add("%s=%s" % (key, val))

        return args

    def getCurrentLangSearchList(self):
        return localization.expand_langs(self.systemLang) + ['C']

    def getDefaultTimeZone(self):
        try:
            return self.localeInfo[self.systemLang][4]
        except KeyError:
            # If doing an upgrade and the system language is something not
            # recognized by anaconda, we should try to see if we can figure
            # it out from the running system.
            if os.path.exists(ROOT_PATH + "/etc/sysconfig/clock"):
                cfg = SimpleConfigFile()
                cfg.read(ROOT_PATH + "/etc/sysconfig/clock")

                try:
                    return cfg.get("ZONE")
                except:
                    return self.localeInfo[self._default][4]
            else:
                return self.localeInfo[self._default][4]

    def textSupported(self, lang):
        try:
            l = self._canonLang(lang)
        except ValueError:
            l = self._default

        return self.localeInfo[l][2]

    def getLangName(self, lang):
        try:
            l = self._canonLang(lang)
        except ValueError:
            l = self._default

        return self.localeInfo[l][0]

    def getLangByName(self, name):
        for (key, val) in self.localeInfo.iteritems():
            if val[0] == name:
                return key

    def getNativeLangName(self, lang):
        return self.nativeLangNames[lang]

    def write(self):
        f = open(ROOT_PATH + "/etc/sysconfig/i18n", "w")

        for (key, val) in self.info.iteritems():
            if val != None:
                f.write("%s=\"%s\"\n" % (key, val))

        f.close()
