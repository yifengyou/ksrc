# 服务端代码
import socket

# 创建一个 TCP 套接字
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
# 绑定本地地址和端口
server.bind(('127.0.0.1', 8888))
# 开始监听连接
server.listen(5)
print('服务器启动，等待客户端连接...')

while True:
    # 接受一个客户端连接，返回一个新的套接字和客户端地址
    client, addr = server.accept()
    print('客户端 {} 已连接'.format(addr))
    while True:
        # 从客户端接收数据，最多 1024 字节
        data = client.recv(1024)
        # 如果没有数据，说明客户端已断开，退出循环
        if not data:
            break
        # 打印接收到的数据
        print('收到', client.getsockname(), '的数据：', data.decode())
        # 向客户端发送数据
        client.send(data)
    # 关闭客户端套接字
    client.close()
    print('客户端 {} 已断开'.format(addr))
# 关闭服务端套接字
server.close()
