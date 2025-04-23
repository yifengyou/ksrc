# 客户端代码
import socket

# 创建一个 TCP 套接字
client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
# 连接服务端地址和端口
client.connect(('127.0.0.1', 8888))
print('已连接服务器')

while True:
    # 输入要发送的数据
    data = input('请输入要发送的数据：')
    # 如果输入为空，退出循环
    if not data:
        break
    # 向服务端发送数据
    client.send(data.encode())
    # 从服务端接收数据，最多 1024 字节
    data = client.recv(1024)
    # 打印接收到的数据
    print('收到服务端的数据：', data.decode())
# 关闭客户端套接字
client.close()