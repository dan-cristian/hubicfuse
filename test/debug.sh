
SRC_FILE=newsrc

function main(){
echo
killall hubicfuse > /dev/null 2>&1
make clean
make debug
sudo umount -l /mnt/hubic2 > /dev/null 2>&1
sudo make install
if [ "$?" == "0" ]; then
	touch newbuild
	rm -Rf /mnt/hubic2/*
	gdb --eval-command=run --args hubicfuse /mnt/hubic2 -o noauto_cache,sync_read,allow_other,big_writes,large_read -f
else
	echo error make
fi
}

echo Waiting for source file changes...
while true; do
sleep 1
if [ -f $SRC_FILE ]; then
	echo New source detected!
	rm $SRC_FILE
	read -t 1 -p "Press a key to compile and run"
	main
	echo Run completed!
	echo ==============
	echo
	echo Waiting for source file changes...
fi
done