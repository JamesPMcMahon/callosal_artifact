#taken from: https://www.geeksforgeeks.org/python/how-to-capture-udp-packets-in-python/
import socket
import sys

# Define the UDP IP address and port to listen on
UDP_PORT = 8888 

if (sys.argv < 2):
       print("FAILED, NEED DURATION AND NEED IP ARGUMENT") 
else:
    UDP_IP = sys.argv[1]
    # Create a UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_IP, UDP_PORT))

    while True:
        # Receive data from the socket
        data, addr = sock.recvfrom(1024)
        print(data)
        break 

