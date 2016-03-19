#!/bin/bash

MOUNT_HUBIC=/mnt/hubic2
TEST1_ROOT=/default/test/$TEST_FOLDER
HUB=$MOUNT_HUBIC$TEST1_ROOT
CACHE_RESET_KEY=/debug-decache
#HUB_NOCACHE=$MOUNT_HUBIC/test/ref
SRC=~/test/ref
HUBIC_TMP=/media/temp/hubicfuse
HUBIC_CFG=~/.hubicfuse
TMP=/media/temp/hubic_test_tmp/$TEST_FOLDER
BUILD_FILE=newbuild
SRC_FILE=newsrc
SMALL=small.txt
MEDIUM=medium.txt
LARGE=large.avi
TINY=tiny.txt
HUGE=huge.mkv
TINY_MD5=41ec28b253670dcc01601014317bece0
LARGE_MD5=701603c35a8b3af176dc687e17e7b44e
SMALL_MD5=70a4b9f4707d258f559f91615297a3ec
MEDIUM_MD5=71a376228c6057f5ca318797dd2dca3c
HUGE_MD5=8d5baa851762166d2e279dceec3b9024
COPY_CMD=cp
#COPY_CMD=rsync -ah --progress

PASSED_MSG="\e[32m PASSED \e[0m"
FAILED_MSG="\e[31m FAILED \e[0m"
COL_YEL="\e[33m"
COL_NORM="\e[0m"

function check()
{
if [ "$?" == "0" ]; then
  echo -e $PASSED_MSG
  return 1
else
  echo -e "$FAILED_MSG [$COL_YEL$1$COL_NORM]"
  return 0
fi
}

function check_not()
{
if [ "$?" == "0" ]; then
  echo -e "$FAILED_MSG $COL_YEL[$1]$COL_NORM"
  return 0
else
  echo -e $PASSED_MSG
  return 1
fi
}

function test(){
  echo -n $(date +"%H:%m:%S")" $! Testing: $1 [$2] ..."
  OUTPUT="$((eval $2 > /dev/null) 2>&1)"
  check "$OUTPUT"
}
export -f test

function test_not(){
  echo -n $(date +"%H:%m:%S")" $! Testing: $1 [$2]..."
  OUTPUT="$((eval $2 > /dev/null) 2>&1)"
  check_not "$OUTPUT"
}
export -f test_not

# $1=file name, $2=target md5sum
function check_md5(){
	echo -n $(date +"%H:%m:%S")" $! Testing: md5sum check" $1 "..."
	md5=$(md5sum $1)
	if [[ "$md5" == *"$2"* ]]; then
		echo -e $PASSED_MSG $!
		return 1
	else
		echo -n " $md5!=$2 "
		echo -e $FAILED_MSG $!
		return 0
	fi
}
export -f check_md5

# $1=file name, $2=target chmod
function check_chmod(){
	echo -n $(date +"%H:%m:%S")" $! Testing: chmod check" $1 "..."
	chmod=$(stat -c %a $1)
	if [[ "$chmod" == *"$2"* ]]; then
		echo -e $PASSED_MSG $!
		return 1
	else
		echo -n " $chmod!=$2 "
		echo -e $FAILED_MSG $!
		return 0
	fi
}
export -f check_chmod

# $1=file name, $2=target chown
function check_chown(){
	echo -n $(date +"%H:%m:%S")" $! Testing: chown check" $1 "..."
	chown=$(stat -c "%U:%G" $1)
	if [[ "$chown" == *"$2"* ]]; then
		echo -e $PASSED_MSG $!
		return 1
	else
		echo -n " $chown!=$2 "
		echo -e $FAILED_MSG $!
		return 0
	fi
}
export -f check_chown

# $1=file name, $2=target time
function check_utimens(){
	echo -n $(date +"%H:%m:%S")" $! Testing: utimens check" $1 "..."
	access=$(stat -c "%x" $1)
	modified=$(stat -c "%y" $1)
	if [[ "$access" == *"$2"* ]]; then
		if [[ "$modified" == *"$2"* ]]; then
			echo -e $PASSED_MSG $!
			return 1
		else
			echo -n " $modified!=$2 "
			echo -e $FAILED_MSG $!
			return 0
		fi
	else
		echo -n " $access!=$2 "
		echo -e $FAILED_MSG $!
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

function delete_fuse_cache(){
	echo "Deleting fuse cache in $HUBIC_TMP"
	rm -Rf $HUBIC_TMP/*
}

function setup_test(){
	echo
	echo Cleaning folders $TMP/* ...
	rm -Rf $TMP/*
	delete_fuse_cache
	rm -Rf $HUB/*
	rmdir $HUB
	
	echo Preparing temp folders...
	mkdir -p $TMP

	if test MKDIR "mkdir $HUB"; then return; fi
	if test RMDIR "rmdir $HUB"; then return; fi
	if test "create test folder" "mkdir $HUB"; then return; fi
}

function cache_reset()
{
	echo "Clearing path cache $1$CACHE_RESET_KEY"
	stat "$1$CACHE_RESET_KEY" > /dev/null 2>&1
}

function test_upload_small(){
	echo "Testing copy operations, upload small files"
	if test "upload non-segmented file" "$COPY_CMD $SRC/$TINY $HUB/"; then return; fi
	if test "upload non-segmented file" "$COPY_CMD $SRC/$SMALL $HUB/"; then return; fi
	
	echo Test completed!
	echo ---------------
	return 1
}

function test_upload_medium(){
	echo "Testing copy operations, upload medium file"
	if test "upload segmented file" "$COPY_CMD $SRC/$MEDIUM $HUB/"; then return; fi
	echo Test completed!
	echo ---------------
	return 1
}

function test_upload_large(){
	if test "upload segmented file" "$COPY_CMD $SRC/$LARGE $HUB/"; then return; fi
	return 1
}

function test_download_small(){
	echo "Testing copy operations, download small files"
	if test "download tiny file" "$COPY_CMD $HUB/$TINY $TMP/"; then return; fi
	if check_md5 "$TMP/$TINY" "$TINY_MD5"; then return; fi
	if test "download small file" "$COPY_CMD $HUB/$SMALL $TMP/"; then return; fi
	if check_md5 "$TMP/$SMALL" "$SMALL_MD5"; then return; fi
	echo Test completed!
	echo ---------------
	return 1
}

function test_download_medium(){
	echo "Testing copy operations, download medium file"
	if test "download medium file" "$COPY_CMD $HUB/$MEDIUM $TMP/"; then return; fi
	if check_md5 "$TMP/$MEDIUM" "$MEDIUM_MD5"; then return; fi
	echo Test completed!
	echo ---------------
	return 1
}

function test_download_medium_copy(){
	echo "Testing copy operations, download 2nd copy medium file "
	if test "download medium file" "$COPY_CMD $HUB/$MEDIUM $TMP/copy$MEDIUM"; then return; fi
	if check_md5 "$TMP/copy$MEDIUM" "$MEDIUM_MD5"; then return; fi
	echo Test completed!
	echo ---------------
	return 1
}

function test_downup_small(){
	echo "Testing copy operations, download and upload "
	if test "download upload tiny file" "$COPY_CMD $HUB/$TINY $HUB/copy$TINY"; then return; fi
	if test "download tiny file" "$COPY_CMD $HUB/copy$TINY $TMP/"; then return; fi
	if check_md5 "$TMP/copy$TINY" "$TINY_MD5"; then return; fi
	
	echo Test completed!
	echo ---------------
	return 1
}

function test_copy_huge()
{
	echo "Testing copy operations, download"
	if test "download large file" "$COPY_CMD $HUB/$LARGE $TMP/"; then return; fi
	if check_md5 "$TMP/$LARGE" "$LARGE_MD5"; then return; fi
	if test "download huge segmented file" "$COPY_CMD $HUB/$HUGE $TMP/"; then return; fi
	if check_md5 "$TMP/$HUGE" "$HUGE_MD5"; then return; fi
	echo Test completed!
	echo ---------------
	return 1
}

function test_chmod(){
	echo "Testing chmod..."
	if test "chmod set" "chmod 765 $HUB/$TINY"; then return 0; fi
	cache_reset
	if check_chmod "$HUB/$TINY" "765"; then return 0; fi
	echo Test completed!
	echo ---------------
	return 1
}

function test_chown(){
	echo "Testing chown..."
	if test "chown set" "chown haiot:dcristian $HUB/$TINY"; then return 0; fi
	cache_reset
	if check_chown "$HUB/$TINY" "haiot:dcristian"; then return 0; fi
	if test "chown set" "chown dcristian:users $HUB/$TINY"; then return 0; fi
	if check_chown "$HUB/$TINY" "dcristian:users"; then return 0; fi
	echo Test completed!
	echo ---------------
	return 1
}

function test_utimens(){
	echo "Testing utimens..."
	if test "utimens set" "touch -a -m -d '2011-03-06 00:15:16.000000000 +0200' $HUB/$TINY"; then return 0; fi
	cache_reset "$HUB/$TINY"
	if check_utimens "$HUB/$TINY" "2011-03-06 00:15:16.000000000 +0200"; then return 0; fi
	#check size to ensure meta was not lost
	if test "download tiny file" "$COPY_CMD $HUB/$TINY $TMP/"; then return; fi
	if check_md5 "$TMP/$TINY" "$TINY_MD5"; then return; fi
	echo Test completed!
	echo ---------------
	return 1
}

function test_rename_small(){
	echo "Testing rename small files..."
	
	if test "rename tiny file" "mv $HUB/$TINY $HUB/renamed$TINY"; then return 0; fi
	if test_not "old file must not exist" "stat $HUB/$TINY"; then return 0; fi
	if test "new file must exist" "stat $HUB/renamed$TINY"; then return 0; fi
	if test "rename tiny file back" "mv $HUB/renamed$TINY $HUB/$TINY"; then return 0; fi
	
	echo "Testing rename small segmented file..."
	if test "rename medium file" "mv $HUB/$MEDIUM $HUB/renamed$MEDIUM"; then return 0; fi
	if test_not "old file must not exist" "stat $HUB/$MEDIUM"; then return 0; fi
	if test "new file must exist" "stat $HUB/renamed$MEDIUM"; then return 0; fi
	if test "rename medium file back" "mv $HUB/renamed$MEDIUM $HUB/$MEDIUM"; then return 0; fi
	delete_fuse_cache
	if test "download medium file" "$COPY_CMD $HUB/$MEDIUM $TMP/"; then return 0; fi
	if check_md5 "$TMP/$MEDIUM" "$MEDIUM_MD5"; then return 0; fi
	
	echo Test completed!
	echo ---------------
	return 1
}

function test_rename_large(){
	echo "Testing rename segmented file..."
	if test "rename large file" "mv $HUB/$LARGE $HUB/renamed$LARGE"; then return 0; fi
	if test_not "old file must not exist" "stat $HUB/$LARGE"; then return 0; fi
	if test "new file must exist" "stat $HUB/renamed$LARGE"; then return 0; fi
	if test "download renamed large file" "$COPY_CMD $HUB/renamed$LARGE $TMP/"; then return 0; fi
	if check_md5 "$TMP/renamed$LARGE" "$LARGE_MD5"; then return 0; fi
	if test "rename large file back" "mv $HUB/renamed$LARGE $HUB/$LARGE"; then return 0; fi
	if test "download large file" "$COPY_CMD $HUB/$LARGE $TMP/"; then return 0; fi
	if check_md5 "$TMP/$LARGE" "$LARGE_MD5"; then return 0; fi
	echo Test completed!
	echo ---------------
	return 1
}

function test_create(){
	echo "Testing create file..."
	if test "create empty file" "touch $HUB/touch$TINY"; then return 0; fi
	if test "new file must exist" "stat $HUB/touch$TINY"; then return 0; fi
	if test "append to new file" "cat $HUB/$TINY >> $HUB/touch$TINY"; then return 0; fi
	if check_md5 "$HUB/touch$TINY" "$TINY_MD5"; then return 0; fi
	echo Test completed!
	echo ---------------
	return 1
}

function test_delete_small(){
	echo "Testing delete file..."
	if test "delete tiny file" "rm $HUB/$TINY"; then return 0; fi
	if test_not "old file must not exist" "stat $HUB/$TINY"; then return 0; fi
	if test "delete small file" "rm $HUB/$SMALL"; then return 0; fi
	if test_not "old file must not exist" "stat $HUB/$SMALL"; then return 0; fi
	if test "delete medium file" "rm $HUB/$MEDIUM"; then return 0; fi
	if test_not "old file must not exist" "stat $HUB/$MEDIUM"; then return 0; fi
	
	echo Test completed!
	echo ---------------
	return 1
}
