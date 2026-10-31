node=$1
bench=$2
#"DRWOH"
duration=$3
offset=$4
#100
ip=$5


while [ 1 -le 10 ]
do
	echo ""
	echo "duration $duration"
	echo "waiting for next threadcount $ip"
	threadcount=$(python python_start.py $ip)
	if [[ "$threadcount" == "9" ]];then
		echo "received 9, done"	
		exit
	fi

	if [[ "$threadcount" == "0" ]];
	then
		date 
		echo "not running this time"
	else
		date
		echo running $node $threadcount $duration $ip
		echo "sudo ./fxmark --type $bench --root /nfs/general --ncore $threadcount --duration $duration --offset $offset"
		sudo ./fxmark --type $bench --root /nfs/general --ncore $threadcount --duration $duration --offset $offset --setup 0
	fi
	echo ""
done

