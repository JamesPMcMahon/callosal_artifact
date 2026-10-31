#do setup first

index="4"
offset="18"

for bench in "DWOH" "DWOL" "DWOM" "DRBH" "DRBL" "DRBM" "DRWOH" "DRWOM"

do
    #sleep to make sure previous run is completely over  
    sleep 10 

    #remove all files in directory
    #rm /mnt/*

    #sleep to make sure of no overlapping deletes 

    #do setup for next run
    echo "bench: $bench"
    #echo "doing setup don't count this run"
    sudo ./fxmark --type $bench --root /nfs/general --ncore 6 --duration 1 --setup 1 --offset 0 

    sleep 10  
    #echo "end of doing setup"

    #begin actual next run
    sudo bash run_nfs.sh 0 $bench 60 $offset IP | tee "results/${bench}_${index}"

done

