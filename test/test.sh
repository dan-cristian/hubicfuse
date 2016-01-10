MOUNT_HUBIC=/mnt/hubic2
HUB_ROOT=$MOUNT_HUBIC/test
#HUB=/mnt/hubic2/test/t1
HUB=$MOUNT_HUBIC/default/test/t2
CACHE_RESET_CMD=$MOUNT_HUBIC/debug-decache
HUB_NOCACHE=$MOUNT_HUBIC/test/ref
SRC=~/test/ref
HUBIC_TMP=/media/temp/hubicfuse
HUBIC_CFG=~/.hubicfuse
TMP=/media/temp/hubic_test_tmp
BUILD_FILE=newbuild
SRC_FILE=newsrc
SMALL=small.txt
LARGE=large.avi
TINY=tiny.txt
HUGE=huge.mkv
TINY_MD5=41ec28b253670dcc01601014317bece0
LARGE_MD5=701603c35a8b3af176dc687e17e7b44e
SMALL_MD5=70a4b9f4707d258f559f91615297a3ec
HUGE_MD5=8d5baa851762166d2e279dceec3b9024
COPY_CMD=cp
#COPY_CMD=rsync -ah --progress

PASSED_MSG="\e[32m PASSED \e[0m"
FAILED_MSG="\e[31m FAILED \e[0m"

function check()
{
if [ "$?" == "0" ]; then
  echo -e $PASSED_MSG
  return 1
else
  echo -e $FAILED_MSG
  return 0
fi
}

function check_not()
{
if [ "$?" == "0" ]; then
  echo -e $FAILED_MSG
  return 0
else
  echo -e $PASSED_MSG
  return 1
fi
}

function test(){
  echo -n "Testing: $1 [$2] ..."
  eval $2 > /dev/null 2>&1
  check
}

function test_not(){
  echo -n "Testing: $1 [$2]..."
  eval $2 > /dev/null 2>&1
  check_not
}

# $1=file name, $2=target md5sum
function test_md5(){
	echo -n "Testing: md5sum check" $1 "..."
	md5=$(md5sum $1)
	if [[ "$md5" == *"$2"* ]]; then
		echo -e $PASSED_MSG
		return 1
	else
		echo -n " $md5!=$2 "
		echo -e $FAILED_MSG
		return 0
	fi
}

# $1=file name, $2=target chmod
function test_chmod(){
	echo -n "Testing: chmod check" $1 "..."
	chmod=$(stat -c %a %1)
	if [[ "$chmod" == *"$2"* ]]; then
		echo -e $PASSED_MSG
		return 1
	else
		echo -n " $chmod!=$2 "
		echo -e $FAILED_MSG
		return 0
	fi
}

function setup_config_progressive(){
	echo
	echo Setting hubicfuse progressive config...
	rm -f $HUBIC_CFG
	cp -f .hubicfuse.progressive $HUBIC_CFG
	cat ~/.hubicfuse.secret >> $HUBIC_CFG
}

function setup_config_standard(){
	echo
	echo Setting hubicfuse standard config...
	rm -f $HUBIC_CFG
	cp -f .hubicfuse.standard $HUBIC_CFG
	cat ~/.hubicfuse.secret >> $HUBIC_CFG
}

function setup_test(){
	echo
	echo Cleaning folders...
	rm -Rf $HUBIC_TMP/*
	rm -Rf $TMP/*
	
	rm -Rf $HUB/*
	rmdir $HUB

	echo Preparing temp folders...
	mkdir -p $TMP
	mkdir -p $HUB_ROOT

	if test MKDIR "mkdir $HUB"; then return; fi
	if test RMDIR "rmdir $HUB"; then return; fi
	if test "create test folder" "mkdir $HUB"; then return; fi
}

function cache_reset()
{
	echo "Clearing fuse driver cache and reloading config (equivalent of restart?)..."
	stat $CACHE_RESET_CMD
}

function test_upload_small(){
	echo "Testing copy operations, upload"
	if test "upload non-segmented file" "$COPY_CMD $SRC/$TINY $HUB/"; then return; fi
	if test "upload non-segmented file" "$COPY_CMD $SRC/$SMALL $HUB/"; then return; fi
	echo Test completed!
	echo ---------------
}

function test_upload_large(){
	if test "upload segmented file" "$COPY_CMD $SRC/$LARGE $HUB/"; then return; fi	
}

function test_download_small(){
	echo "Testing copy operations, download"
	if test "download tiny file" "$COPY_CMD $HUB_NOCACHE/$TINY $TMP/"; then return; fi
	if test_md5 "$TMP/$TINY" "$TINY_MD5"; then return; fi
	if test "download small file" "$COPY_CMD $HUB/$SMALL $TMP/"; then return; fi
	if test_md5 "$TMP/$SMALL" "$SMALL_MD5"; then return; fi
	echo Test completed!
	echo ---------------
}

function test_copy_huge()
{
	echo "Testing copy operations, download"
	if test "download large file" "$COPY_CMD $HUB_NOCACHE/$LARGE $TMP/"; then return; fi
	if test_md5 "$TMP/$LARGE" "$LARGE_MD5"; then return; fi
	if test "download huge segmented file" "$COPY_CMD $HUB_NOCACHE/$HUGE $TMP/"; then return; fi
	if test_md5 "$TMP/$HUGE" "$HUGE_MD5"; then return; fi
}

function test_chmod(){
	echo "Testing chmod..."
	if test "chmod set" "chmod 765 $HUB_NOCACHE/$TINY"; then return 0; fi
	cache_reset
	if test_chmod "$HUB_NOCACHE/$TINY" "765"; then return 0; fi
	return 1
}

function test_rename(){
	echo "Testing rename..."
	if test "rename tiny file" "mv $HUB/$TINY $HUB/renamed$TINY"; then return 0; fi
	if test_not "old file must not exist" "stat $HUB/$TINY"; then return 0; fi
	if test "new file must exist" "stat $HUB/renamed$TINY"; then return 0; fi
	if test "rename tiny file back" "mv $HUB/renamed$TINY $HUB/$TINY"; then return 0; fi
	return 1
}

echo Waiting for a new build...
setup_config_progressive

while true; do
	sleep 1
	if [ -f $SRC_FILE ]; then
		#setup_config_standard
		echo Detected new source file!
		#killall hubicfuse > /dev/null 2>&1
		#killall gdb > /dev/null 2>&1
		sleep 2
	fi
	
	if [ -f $BUILD_FILE ]; then
		echo New build detected!
		rm $BUILD_FILE
		sleep 5
		setup_test
		
		test_upload_small
		if test_chmod; then exit; fi
		if test_rename; then exit; fi
		
		test_upload_small
		
		
		
		test_download_small
		test_download_small
		
		echo "Check copy consistency, older segments should be removed..."
		
		cache_reset
		
		echo ===============
		echo
		echo Waiting for a new build...
	fi
done