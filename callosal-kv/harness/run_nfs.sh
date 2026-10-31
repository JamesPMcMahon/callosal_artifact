node=$1
node_count=$2
#"DRWOH"
duration=$3
#100
ip=$4

loop=$5

records=$6
#21504
filepages=$7
#64

dists=("zipf" "uniform")
ratios=("0.0" "0.01" "0.05")



#239400_500

#echo "doing populate"
sudo ../callosal-kv --populate \
	--path /nfs/general \
	--records $records \
	--node-id $node \
	--num-nodes $node_count \
	--seed 1 \
	--load-factor 75 \
	--max-file-pages $filepages
echo "done populate"

echo sudo ../callosal-kv --populate \
	--path /nfs/general \
	--records $records \
	--node-id $node \
	--num-nodes $node_count \
	--seed 1 \
	--load-factor 75 \
	--max-file-pages $filepages



for dist in "${dists[@]}"
do
	for ratio in "${ratios[@]}"
	do
		echo "-----------------------------------------------"
		echo "-----------------------------------------------"
		echo "-----------------------------------------------"
		echo "-----------------------------------------------"
		echo "-----------------------------------------------"

		echo "running ${dist} ${ratio} with ${records} records and ${filepages} pages per page"

		echo "-----------------------------------------------"
		echo "-----------------------------------------------"
		echo "-----------------------------------------------"
		echo "-----------------------------------------------"
		echo "-----------------------------------------------"

		mkdir -p ${records}_${filepages}_${dist}_${ratio}_results
		i=1
		j=1
		#loop for total amount
		#3 = (4-1)
		while [ "$j" -le $loop ]
		do

			echo ""
			echo "duration $duration"
			echo "waiting for next threadcount $ip"
			threadcount=$(python python_start.py $ip)
			if [[ "$threadcount" == "0" ]];
			then
				date
				echo "not running for this iteration" > ${records}_${filepages}_${dist}_${ratio}_results/"node${node}-r${records}-iter${i}-t${threadcount}.json"
			else
				date
				echo running $threadcount $ip
				echo running $node $duration $ip threadcount $threadcount
				sudo ../callosal-kv --run --path /nfs/general \
					--records $records \
					--node-id $node \
					--num-nodes $node_count \
					--seed 1 \
					--load-factor 75 \
					--max-file-pages $filepages \
					--duration $duration \
					--threads $threadcount \
					--dist $dist \
					--write-ratio $ratio \
					--probe monotonic \
					--out ${records}_${filepages}_${dist}_${ratio}_results/"node${node}-r${records}-iter${i}-t${threadcount}.json"

				echo sudo ../callosal-kv --run --path /nfs/general \
                                        --records $records \
                                        --node-id $node \
                                        --num-nodes $node_count \
                                        --seed 1 \
                                        --load-factor 75 \
                                        --max-file-pages $filepages \
                                        --duration $duration \
                                        --threads $threadcount \
                                        --dist $dist \
                                        --write-ratio $ratio \
                                        --probe monotonic \
                                        --out ${records}_${filepages}_${dist}_${ratio}_results/"node${node}-r${records}-iter${i}-t${threadcount}.json"
				#results/"node${node}-r${records}-iter${i}-t${threadcount}.json"


				echo ""
			fi
			i=$((i + 1))
			j=$((j + 1))
		done
	done
done
echo done

#sudo bash run_script.sh 2 4 60 IP 4 239400 500

