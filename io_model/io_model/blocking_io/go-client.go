package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"strings"
)

func main() {
	// 连接到服务器，地址为localhost:8080
	conn, err := net.Dial("tcp", "localhost:8080")
	if err != nil {
		fmt.Println("Error dialing:", err)
		return
	}
	defer conn.Close() // 关闭连接

	fmt.Println("Connected to the server:", conn.RemoteAddr())

	reader := bufio.NewReader(os.Stdin) // 创建一个从标准输入读取数据的读取器

	for {
		fmt.Print("Enter a message: ") // 提示用户输入消息

		input, _ := reader.ReadString('\n') // 读取用户输入的一行数据

		if input == "exit\n" { // 如果用户输入exit，则退出循环
			break
		}

        input = strings.TrimSuffix(input, "\n") // 删除结尾的换行符

		n, err := conn.Write([]byte(input)) // 将用户输入的数据写入连接，阻塞直到发送完成
		if err != nil {
			fmt.Println("Error writing:", err)
			break
		}

		fmt.Println("Sent to server:", input[:n-1])

		buffer := make([]byte, 1024) // 创建一个缓冲区

		n, err = conn.Read(buffer) // 从连接中读取数据，阻塞直到有数据到达
		if err != nil {
			fmt.Println("Error reading:", err)
			break
		}

		fmt.Println("Received from server:", string(buffer[:n]))

    }

    fmt.Println("Disconnected from the server.")
}
