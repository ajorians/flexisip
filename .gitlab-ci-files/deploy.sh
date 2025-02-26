#!/bin/bash

function print_usage {
	prog=$(basename $0)
	echo "syntax: $prog <dist>" 1>&2
	exit 2
}

if [ -z "$1" ] || [ "${1:0:1}" = '-' ]; then
	print_usage $0
fi

if [ $# -ne 1 ]; then
	print_usage $0
fi

dist="$1"


id=$(cat /dev/urandom | tr -dc '[:alnum:]' | fold -w 10 | head -n 1)
tmpdir="$MAKE_REPO_TMP/tmp-$id"
rsync_dest="$DEPLOY_SERVER:$tmpdir/"

case "$dist" in
	'centos')
		make_repo_args="rpm $tmpdir $CENTOS_REPOSITORY"
		rsync_src='build/*.rpm'
		;;
  'rockylinux')
		make_repo_args="rpm $tmpdir $ROCKYLINUX_REPOSITORY"
		rsync_src='build/*.rpm'
		;;
	'debian')
		make_repo_args="deb $tmpdir $FREIGHT_PATH $RELEASE"
		echo "make_repo_args=$make_repo_args"
		rsync_src='build/*.deb build/*.ddeb'
		;;
	       *)
		echo "invalid distribution type: '$dist'. Only 'centos', 'rockylinux' and 'debian' are valid" 1>&2
		exit 2
		;;
esac

echo ">>> Pushing packages into '$rsync_dest'"
rsync -v $rsync_src $rsync_dest

echo ">>> Connecting on '$DEPLOY_SERVER'"
ssh $DEPLOY_SERVER "
	echo '>>>> Making repository'
	make_repo $make_repo_args || exit 1

	echo \">>>> Removing '$tmpdir'\"
	rm -r $tmpdir
"

