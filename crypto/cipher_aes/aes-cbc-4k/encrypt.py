import os
from Crypto.Cipher import AES


def encrypt_4k_chunks(data: bytes, password: str) -> bytes:
    """
    纯4K块加密（无文件头）
    每个块独立加密，不足4K补零

    参数:
        data: 原始数据
        password: 加密密码

    返回:
        加密后的数据（长度必为4096的倍数）
    """
    key = password.encode().ljust(32)[:32]
    cipher = AES.new(key, AES.MODE_ECB)
    block_size = 4096

    # 计算需要填充到多少个4K块
    num_blocks = (len(data) + block_size - 1) // block_size
    encrypted = b""

    for i in range(num_blocks):
        chunk = data[i * block_size: (i + 1) * block_size]
        # 最后一个块不足4K时补零
        if len(chunk) < block_size:
            chunk = chunk.ljust(block_size, b'\x00')
        encrypted += cipher.encrypt(chunk)

    return encrypted


def decrypt_4k_chunks(encrypted_data: bytes, password: str, original_size: int) -> bytes:
    """
    解密4K块并截断到原始大小

    参数:
        encrypted_data: 加密数据（必须4096对齐）
        password: 解密密码
        original_size: 原始数据长度

    返回:
        解密后的原始数据
    """
    key = password.encode().ljust(32)[:32]
    cipher = AES.new(key, AES.MODE_ECB)
    block_size = 4096

    if len(encrypted_data) % block_size != 0:
        raise ValueError("加密数据长度必须是4096的倍数")

    decrypted = b""
    for i in range(0, len(encrypted_data), block_size):
        decrypted += cipher.decrypt(encrypted_data[i:i + block_size])

    # 截断到原始大小（去除补零部分）
    return decrypted[:original_size]


def encrypt_file(input_path: str, output_path: str, password: str):
    """加密文件（输出必为4K倍数）"""
    with open(input_path, 'rb') as f:
        data = f.read()

    encrypted = encrypt_4k_chunks(data, password)

    with open(output_path, 'wb') as f:
        f.write(encrypted)


def decrypt_file(input_path: str, output_path: str, password: str, original_size: int):
    """解密文件（需提供原始大小）"""
    with open(input_path, 'rb') as f:
        encrypted = f.read()

    decrypted = decrypt_4k_chunks(encrypted, password, original_size)

    with open(output_path, 'wb') as f:
        f.write(decrypted)


if __name__ == "__main__":
    # 测试用例
    TEST_PASSWORD = "MySecurePassword123!"
    TEST_FILE = "test.txt"  # 原始文件
    ENCRYPTED_FILE = "test.enc"  # 加密后
    DECRYPTED_FILE = "test.dec"  # 解密后

    # # 创建测试文件（5字节）
    # with open(TEST_FILE, 'w') as f:
    #     f.write("hello")

    # 加密测试
    original_size = os.path.getsize(TEST_FILE)
    encrypt_file(TEST_FILE, ENCRYPTED_FILE, TEST_PASSWORD)

    print(f"原始文件大小: {original_size} 字节")
    print(f"加密后大小: {os.path.getsize(ENCRYPTED_FILE)} 字节 (4K对齐)")

    # 解密测试（需手动传入原始大小）
    decrypt_file(ENCRYPTED_FILE, DECRYPTED_FILE, TEST_PASSWORD, original_size)

