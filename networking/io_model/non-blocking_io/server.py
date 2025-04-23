# 服务端
import socket
import time

server=socket.socket()
server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
server.bind(('127.0.0.1',8083))
server.listen(5)

server.setblocking(False)
r_list=[]
w_list={}

while 1:
    try:
        conn,addr=server.accept()
        r_list.append(conn)
    except BlockingIOError:
        # 强调强调强调：！！！非阻塞IO的精髓在于完全没有阻塞！！！
        print('在做其他的事情')
        print('rlist: ',len(r_list))
        print('wlist: ',len(w_list))
        time.sleep(0.5) # 打开该行注释纯属为了方便查看效果


        # 遍历读列表，依次取出套接字读取内容
        del_rlist=[]
        for conn in r_list:
            try:
                data=conn.recv(1024)
                if not data:
                    conn.close()
                    del_rlist.append(conn)
                    continue
                w_list[conn]=data.upper()
            except BlockingIOError: # 没有收成功，则继续检索下一个套接字的接收
                continue
            except ConnectionResetError: # 当前套接字出异常，则关闭，然后加入删除列表，等待被清除
                conn.close()
                del_rlist.append(conn)


        # 遍历写列表，依次取出套接字发送内容
        del_wlist=[]
        for conn,data in w_list.items():
            try:
                conn.send(data)
                del_wlist.append(conn)
            except BlockingIOError:
                continue


        # 清理无用的套接字,无需再监听它们的IO操作
        for conn in del_rlist:
            r_list.remove(conn)

        for conn in del_wlist:
            w_list.pop(conn)
