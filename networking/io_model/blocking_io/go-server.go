package main

import (
	"fmt"
	"net"
)

func main() {
	// 创建一个TCP监听器，绑定本地端口8080
	listener, err := net.Listen("tcp", "localhost:8080")
	if err != nil {
		fmt.Println("Error listening:", err)
		return
	}
	defer listener.Close() // 关闭监听器

	fmt.Println("Server is listening on port 8080...")

	for {
		// 接受客户端的连接请求
		conn, err := listener.Accept()
		if err != nil {
			fmt.Println("Error accepting:", err)
			continue
		}

		fmt.Println("Connected to a client:", conn.RemoteAddr())

		// 创建一个goroutine来处理每个客户端的通信
		go handleConnection(conn)
		//handleConnection(conn)
	}
}

// handleConnection函数处理每个客户端的通信
func handleConnection(conn net.Conn) {
	defer conn.Close() // 关闭连接

	buffer := make([]byte, 1024) // 创建一个缓冲区

	for {
		// 从连接中读取数据，阻塞直到有数据到达
		n, err := conn.Read(buffer)
		if err != nil {
			fmt.Println("Error reading:", err)
			break
		}

		fmt.Println("Received from ", conn.RemoteAddr(), ":[", string(buffer[:n]), "]")

		// 将收到的数据原样写回给客户端，阻塞直到发送完成
		n, err = conn.Write(buffer[:n])
		if err != nil {
			fmt.Println("Error writing:", err)
			break
		}

		fmt.Println("Sent to ", conn.RemoteAddr(), ":[", string(buffer[:n]), "]")
	}
}
