#客户端
from socket import *

client=socket(AF_INET,SOCK_STREAM)
client.connect(('127.0.0.1',8093))


while True:
    msg=input('>>: ').strip()
    if not msg:continue
    client.send(msg.encode('utf-8'))
    data=client.recv(1024)
    print(data.decode('utf-8'))

client.close()