#!/bin/sh

DEBRELEASE=$(head -n1 debian/changelog | cut -d ' ' -f 2 | sed 's/[()]*//g')

TMPDIR=/tmp/jack-peak-${DEBRELEASE}
rm -rf ${TMPDIR}

git-buildpackage \
	--git-upstream-branch=master --git-debian-branch=master \
	--git-upstream-tree=branch \
	--git-export-dir=${TMPDIR} --git-cleaner=/bin/true \
	--git-force-create \
	-rfakeroot $@ \
	|| exit

lintian -i --pedantic ${TMPDIR}/jack-peak_${DEBRELEASE}_*.changes \
	| tee /tmp/jrec.issues

echo
ls ${TMPDIR}/jack-peak_${DEBRELEASE}_*.changes
echo
echo dput rg42 ${TMPDIR}/jack-peak_${DEBRELEASE}_*.changes
