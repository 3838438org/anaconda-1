from installclass import BaseInstallClass
from translate import N_
import os
import iutil

class InstallClass(BaseInstallClass):
    name = "upgradeonly"
    pixmap = ""
    hidden = 1
    sortPriority = 1

    def requiredDisplayMode(self):
        return 't'

    def setSteps(self, dispatch):
	dispatch.setStepList(
                    "findrootparts",
		    "findinstall",
                    "partitionobjinit",
                    "upgrademount",
                    "upgradeswapsuggestion",
		    "addswap",
                    "upgrademigfind",
                    "upgrademigratefs",
                    "upgradecontinue",
                    "upgbootloader",
                    "bootloadersetup",
		    "bootloader",
                    "bootloaderpassword",
                    "readcomps",
                    "findpackages",
                    "checkdeps",
		    "dependencies",
		    "confirmupgrade",
		    "install",
                    "migratefilesystems",                    
                    "preinstallconfig",
                    "installpackages",
                    "postinstallconfig",
                    "instbootloader",
                    "dopostaction",
		    "bootdisk",
		    "complete"
		)

        if iutil.getArch() == "alpha" or iutil.getArch() == "ia64":
	    dispatch.skipStep("bootdisk")
            dispatch.skipStep("bootloader")
            dispatch.skipStep("bootloaderpassword")

    def setInstallData(self, id):
        BaseInstallClass.setInstallData(self, id)
        id.upgrade.set(1)
    
    def __init__(self, expert):
	BaseInstallClass.__init__(self, expert)

        self.installType = "upgrade"
