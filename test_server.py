import socket
import time
import subprocess
import sys

def run_tests():
    print("[Test] Starting TCP Server process...")
    server_proc = subprocess.Popen(["/Users/shengsd/github/tcp_server/build/tcp_server_example"])
    time.sleep(1) # 等待服务器启动监听 8888 端口

    try:
        # 测试 1：建立连接与 Echo 接收测试
        print("[Test 1] Testing TCP Connection & Echo...")
        s1 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s1.connect(("127.0.0.1", 8888))
        
        # 接收 Welcome 消息
        welcome = s1.recv(1024).decode('utf-8')
        print(f"Received from server: {welcome.strip()}")
        assert "Welcome" in welcome, "Welcome message mismatch"

        # 发送测试数据
        s1.sendall(b"Hello TCP Server\n")
        echo_reply = s1.recv(1024).decode('utf-8')
        print(f"Echo response: {echo_reply.strip()}")
        assert "[Echo] Hello TCP Server" in echo_reply, "Echo response mismatch"

        # 测试 2：并发连接测试
        print("[Test 2] Testing multiple concurrent connections...")
        clients = []
        for i in range(5):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(("127.0.0.1", 8888))
            s.recv(1024) # read welcome
            clients.append(s)
        
        for idx, s in enumerate(clients):
            msg = f"Client #{idx} ping\n"
            s.sendall(msg.encode('utf-8'))
            reply = s.recv(1024).decode('utf-8')
            assert f"[Echo] Client #{idx} ping" in reply
        
        for s in clients:
            s.close()
        print("[Test 2] Multi-client test passed.")

        # 测试 3：心跳超时检测测试 (设置了15秒超时，保持连接静默)
        print("[Test 3] Testing heartbeat timeout (waiting 17 seconds for idle disconnect)...")
        s3 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s3.connect(("127.0.0.1", 8888))
        s3.recv(1024) # read welcome
        
        # 不发包，等待 17s (超时为 15s)
        time.sleep(17)
        
        # 尝试读取，应该触发 FIN 即 recv 返回 b""
        data = s3.recv(1024)
        assert data == b"", "Connection should be closed by server due to heartbeat timeout"
        s3.close()
        print("[Test 3] Heartbeat timeout test passed!")

        print("\nAll integration tests passed successfully!")

    finally:
        server_proc.terminate()
        server_proc.wait()

if __name__ == "__main__":
    run_tests()
