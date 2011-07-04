# Copyright (C) 2010
# Red Hat, Inc.  All rights reserved.
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
# Author(s): Martin Sivak <msivak@redhat.com>

from StringIO import StringIO
import fnmatch
import glob
import os.path

_orig_glob_glob = glob.glob
_orig_open = open
_orig_os_listdir = os.listdir
_orig_os_path_exists = os.path.exists

class DiskIO(object):
    """Simple object to simplify mocking of file operations in Mock
       based testing"""

    class TestFile(StringIO):
        def __init__(self, store, path, content = ""):
            StringIO.__init__(self, content)
            self._store = store
            self._path = path
            self._ro = False

        def flush(self):
            self._store[self._path] = self.getvalue()

        def close(self):
            self.flush()
            return StringIO.close(self)

        def __del__(self):
            try:
                self.close()
            except (AttributeError):
                pass

        def __enter__(self):
            return self

        def __exit__(self, *_):
            self.close()

    class Dir(object):
        pass

    class Link(object):
        pass

    def __init__(self):
        self.reset()

    def __getitem__(self, key):
        return self.fs[key]

    def __setitem__(self, key, value):
        self.fs[key] = value

    def reset(self):
        self.fs = {
            "/proc": self.Dir,
            "/proc/cmdline": "linux",
        }
        self._pwd = "/"

    #Emulate file objects
    def open(self, filename, mode = "r"):
        path = os.path.join(self._pwd, filename)
        content = self.fs.get(path, None)
        if content == self.Dir:
            raise IOError("[Errno 21] Is a directory: '%s'" % (path))
        elif mode.startswith("w"):
            self.fs[path] = ""
            f = self.TestFile(self.fs, path, self.fs[path])
        elif mode.endswith("a"):
            if not path in self.fs:
                self.fs[path] = ""
            f = self.TestFile(self.fs, path, self.fs[path])
            f.seek(0, os.SEEK_END)
        elif content == None:
            raise IOError("[Errno 2] No such file or directory: '%s'" % (path,))
        elif mode.endswith("+"):
            f = self.TestFile(self.fs, path, content)
            if mode.startswith('r'):
                f.seek(0, os.SEEK_SET)
            else:
                f.seek(0, os.SEEK_END)
        else:
            f = self.TestFile(self.fs, path, content)

        return f

    #Emulate os calls
    def glob_glob(self, pattern):
        return fnmatch.filter(self.fs.keys(), pattern)

    def os_listdir(self, path):
        return [entry[len(path):].lstrip('/') for entry in self.fs.keys()\
                    if entry.startswith(path) and entry != path]

    def os_path_exists(self, path):
        path = os.path.join(self._pwd, path)
        return self.fs.has_key(path)

    def os_remove(self, path):
        path = os.path.join(self._pwd, path)
        try:
            del self.fs[path]
        except KeyError:
            raise OSError("[Errno 2] No such file or directory: '%s'" % (path,))

    def os_access(self, path, mode):
        return self.path_exists(path)

    def take_over_module(self, module):
        """ Trick module into using this object as the filesystem.

            This is achieved by overriding the module's 'open' binding as well
            as some bindings in os.path.
        """
        module.open = self.open
        module.glob.glob = self.glob_glob
        module.os.listdir = self.os_listdir
        module.os.path.exists = self.os_path_exists

    @staticmethod
    def restore_module(module):
        module.open = _orig_open
        module.glob.glob = _orig_glob_glob
        module.os.listdir = _orig_os_listdir
        module.os.path.exists = _orig_os_path_exists
