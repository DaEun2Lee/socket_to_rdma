# RDMA exmaple

A simple RDMA server client example. The code contains a lot of comments. Here is the workflow that happens in the example:

Client:
	1. setup RDMA resources
	2. connect to the server
	3. receive server side buffer information via send/recv exchange
	4. do an RDMA write to the server buffer from a (first) local buffer. The content of the buffer is the string passed with the `-s` argument.
	5. do an RDMA read to read the content of the server buffer into a second local buffer.
	6. compare the content of the first and second buffers, and match them.
	7. disconnect

Server:
	1. setup RDMA resources
	2. wait for a client to connect
	3. allocate and pin a server buffer
	4. accept the incoming client connection
	5. send information about the local server buffer to the client
	6. wait for disconnect


# Authors

## Author of the original code:
	Animesh Trivedi
	atrivedi@apache.org
## Author: 
	Daeun Lee
	delee0929@gmail.com
