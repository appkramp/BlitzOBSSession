#!/bin/sh
# Writes the component property list pkgbuild gets for the plugin bundle:
#   component-plist.sh <payload root> <output plist>
#
# By default Installer refuses to put a bundle over a newer one, and it may
# install a bundle where it finds another copy with the same identifier. The
# installer has already asked the user (distribution.in) and removed the old
# copy (scripts/preinstall), so neither may happen: version checking and
# relocation are turned off for every bundle in the payload.
set -e
root="$1"
out="$2"
/usr/bin/pkgbuild --analyze --root "$root" "$out" >/dev/null
# PlistBuddy's Set needs the key to exist; analyze leaves some out.
put() {
	/usr/libexec/PlistBuddy -c "Delete :$1:$2" "$out" >/dev/null 2>&1 || true
	/usr/libexec/PlistBuddy -c "Add :$1:$2 $3 $4" "$out"
}
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$out" >/dev/null 2>&1; do
	put "$i" BundleIsVersionChecked bool false
	put "$i" BundleIsRelocatable bool false
	put "$i" BundleOverwriteAction string upgrade
	i=$((i + 1))
done
