#do setup first

index="4"
offset="18"
#for bench in "DWOH" "DWOL" "DWOM" "DRBH" "DRBL" "DRBM" "DRWOH" "DRWOM"

for bench in  "DRBH" "DRBL" "DRBM" "DWOH" "DWOL" "DWOM" "DRWOH" "DRWOM"

do
    #sleep to make sure previous run is completely over  
   
    echo only run setup on 1 node 
    sudo ./fxmark --type $bench --root /mnt --ncore 6 --duration 1 --setup 1 --offset 0 
    sleep 10 

    #remove all files in directory
    #rm /mnt/*

    #sleep to make sure of no overlapping deletes 
    #sleep 10 

    #do setup for next run
    echo "bench: $bench"
    #echo "doing setup don't count this run"
    #echo "end of doing setup"

    #begin actual next run
    sudo bash run_script.sh 0 $bench 60 $offset IP | tee "sep25percentage_new/${bench}_${index}"

done


