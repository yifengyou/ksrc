import os
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.backends import default_backend
from binascii import hexlify

# Constants matching the kernel module
AES_KEY_SIZE = 32  # 256 bits
DATA_SIZE = 4096   # 4K data
AES_BLOCK_SIZE = 16  # AES block size (for IV)

# Key must match the kernel module's key
KEY = b"0123456789abcdef0123456789abcdef"  # 32-byte key

def encrypt_aes_ctr(plaintext, iv):
    """
    Encrypt data using AES-256 CTR mode
    Parameters:
        plaintext: bytes to encrypt
        iv: initialization vector (16 bytes)
    Returns:
        ciphertext: encrypted bytes
    """
    if len(KEY) != AES_KEY_SIZE:
        raise ValueError(f"Key must be {AES_KEY_SIZE} bytes")
    if len(iv) != AES_BLOCK_SIZE:
        raise ValueError(f"IV must be {AES_BLOCK_SIZE} bytes")

    cipher = Cipher(
            algorithms.AES(KEY),
            modes.CTR(iv),
            backend=default_backend()
            )
    encryptor = cipher.encryptor()
    ciphertext = encryptor.update(plaintext) + encryptor.finalize()
    return ciphertext

def decrypt_aes_ctr(ciphertext, iv):
    """
    Decrypt data using AES-256 CTR mode (same as encryption in CTR mode)
    Parameters:
        ciphertext: bytes to decrypt
        iv: initialization vector (16 bytes)
    Returns:
        plaintext: decrypted bytes
    """
    # In CTR mode, decryption is the same as encryption
    return encrypt_aes_ctr(ciphertext, iv)

def test_aes():
    """Test function that demonstrates encryption/decryption cycle"""
    # Generate random plaintext
    plaintext = b'\x59\x0a' + b"\0" * 4094
    print(f"Generated {DATA_SIZE} bytes of random data")
    print(f"Plaintext (first 128 bytes): {hexlify(plaintext[:128]).decode()}")

    # Generate random IV
    #iv = os.urandom(AES_BLOCK_SIZE)
    iv = b'\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f'
    print(f"IV: {hexlify(iv).decode()}")

    # Encrypt
    ciphertext = encrypt_aes_ctr(plaintext, iv)
    print(f"Ciphertext (first 128 bytes): {hexlify(ciphertext[:128]).decode()}")

    # Decrypt
    decrypted = decrypt_aes_ctr(ciphertext, iv)
    print(f"Decrypted (first 128 bytes): {hexlify(decrypted[:128]).decode()}")

    # Verify
    if plaintext == decrypted:
        print(f"Verification successful - all {DATA_SIZE} bytes match")
    else:
        print("Decrypted data doesn't match original!")
        return False

    return True

if __name__ == "__main__":
    if test_aes():
        print("AES-256 CTR test completed successfully")
    else:
        print("AES-256 CTR test failed")
