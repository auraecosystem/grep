#!/bin/sh
# With each new upstream version, debian/generated.list is regenerated
# from upstream's .cvsignore files.  The commands to do this follow.
#
# Requires .cvsignore files from the current revision (these are not
# present in the release tarballs).

set -e

find . -name .git -prune -o -type d -exec test -e "{}/.cvsignore" ';' -print |
while read dir
do
	sed "s,^,$dir/," "$dir/.cvsignore"
done > debian/generated.list
