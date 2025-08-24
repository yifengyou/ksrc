#!/usr/bin/env python3

import os
import hashlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.backends import default_backend
from binascii import hexlify

# 配置参数
AES_KEY_SIZE = 32  # 256 bits
DATA_SIZE = 4096   # 4K data
IV_SIZE = 12       # GCM推荐使用12字节IV
AUTH_TAG_SIZE = 16 # 认证标签长度(16字节)

def print_hex(label, data, length=128):
    """打印十六进制数据"""
    hex_data = hexlify(data[:length]).decode('ascii')
    hex_str = ' '.join([hex_data[i:i+2] for i in range(0, len(hex_data), 2)])
    print(f"{label} (first {length} bytes): {hex_str}")

def test_aes_gcm():
    # 1. 准备密钥和IV
    key = b"0123456789abcdef0123456789abcdef"  # 32字节密钥
    iv = os.urandom(IV_SIZE)  # 随机生成IV
    
    # 2. 生成随机明文
    plaintext = os.urandom(100) + b'\0' * 3996
    print(f"Generated {DATA_SIZE} bytes of random data")
    print_hex("plaintext", plaintext)
    
    # 3. 加密
    cipher = Cipher(
        algorithms.AES(key),
        modes.GCM(iv),
        backend=default_backend()
    )
    encryptor = cipher.encryptor()
    ciphertext = encryptor.update(plaintext) + encryptor.finalize()
    auth_tag = encryptor.tag  # 获取认证标签
    
    print("Encryption successful")
    print_hex("ciphertext", ciphertext)
    print_hex("auth tag", auth_tag, AUTH_TAG_SIZE)
    
    # 4. 解密 (正常情况)
    try:
        cipher = Cipher(
            algorithms.AES(key),
            modes.GCM(iv, auth_tag),
            backend=default_backend()
        )
        decryptor = cipher.decryptor()
        decrypted = decryptor.update(ciphertext) + decryptor.finalize()
        
        print("Decryption successful")
        print_hex("decrypted", decrypted)
        
        # 验证
        if plaintext == decrypted:
            print(f"Verification successful - all {DATA_SIZE} bytes match")
        else:
            print("Decrypted data doesn't match original!")
            return False
    
    except Exception as e:
        print(f"Authentication failed: {str(e)}")
        return False
    
    # 5. 测试篡改检测 (故意修改密文)
    try:
        tampered_ciphertext = bytearray(ciphertext)
        tampered_ciphertext[10] ^= 0x01  # 修改一个字节
        
        cipher = Cipher(
            algorithms.AES(key),
            modes.GCM(iv, auth_tag),
            backend=default_backend()
        )
        decryptor = cipher.decryptor()
        decrypted = decryptor.update(tampered_ciphertext) + decryptor.finalize()
        
        # 这行不应该执行到
        print("Tampered data decrypted (this should not happen!)")
        return False
    
    except Exception as e:
        print(f"Expected authentication failure detected: {str(e)}")
    
    return True

if __name__ == "__main__":
    print("AES-256 GCM Python Implementation")
    if test_aes_gcm():
        print("All tests passed successfully")
    else:
        print("Tests failed")
