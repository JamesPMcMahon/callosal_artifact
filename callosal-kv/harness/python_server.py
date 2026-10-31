#taken from: https://www.geeksforgeeks.org/python/how-to-capture-udp-packets-in-python/

import socket
import time
import sys

for i in range(0, 6): 
    print("current iteration " + str(i))
    if (len(sys.argv) < 6):
           print("NEED DURATION ARGUMENT AND WAIT TIME AND IP ADDRESSES") 
    else:
        duration = sys.argv[1]
        waittime = int(duration) + 20
        print("duration is " + str(duration))
        print("wait time is " + str(waittime))
        IPs = [sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]]

        #threadcounts = [
        #        [1,2,3,4,5,6,4,4,5,5,6,6,5,5,5,6,6,6,5,5,6,6,6,6],
        #        [0,0,0,0,0,0,3,4,4,5,5,6,4,5,5,5,6,6,5,5,5,6,6,6],
        #        [0,0,0,0,0,0,0,0,0,0,0,0,4,4,5,5,5,6,5,5,5,5,6,6],
        #        [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,5,5,5,5,6]
        #        ]
        threadcounts = [
                [6,4,4,5,5,6,6],
                [0,3,4,4,5,5,6],
                [0,0,0,0,0,0,0],
                [0,0,0,0,0,0,0]
                ]
        threadcounts = [
                [4,4,5,5,6,6],
                [3,4,4,5,5,6],
                [0,0,0,0,0,0],
                [0,0,0,0,0,0]
                ]

        #threadcounts = [
        #        [6,6],
        #        [0,6],
        #        [0,0],
        #        [0,0]
        #        ]

        #threadcounts = [
        #        [1,2,3,4,5,6],
        #        [0,0,0,0,0,0],
        #        [0,0,0,0,0,0],
        #        [0,0,0,0,0,0]
         #       ]
        threadcounts = [
                [5,5,5,6,6,6],
                [4,5,5,5,6,6],
                [4,4,5,5,5,6],
                [0,0,0,0,0,0]
                ]


        #ports = [8888,9999]
        port = 8888

        for combo in range(0, len(threadcounts[0])):
            print("")
            for i in range(0, len(IPs)):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

                #try:
                thread_count = str(threadcounts[i][combo])
                sock.sendto(thread_count.encode(), (IPs[i], port))
                print('sent ' + str(threadcounts[i][combo]) + " to " + str(IPs[i]))
                sock.close()

                #finally:
                 
            time.sleep(waittime)

