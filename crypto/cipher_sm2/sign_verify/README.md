


```
[73087.932679] kdev: Encrypt test
[73087.934293] kdev: Encrypt crypto_alloc_akcipher ok
[73087.936129] kdev: Encrypt akcipher_request_alloc ok
[73087.937962] kdev: Encrypt set public key ok
[73087.939640] Using cipher: sm2
[73087.941046] Encrypting 10 bytes with public key
[73087.942808] PLAINTEXT: 00000000: 6c 69 6e 75 78 2d 6b 64 65 76                    linux-kdev
[73087.945413] Public key encryption failed: -38
[73088.085636] kdev: Encrypt test
[73088.086075] kdev: Encrypt crypto_alloc_akcipher ok
[73088.086536] kdev: Encrypt akcipher_request_alloc ok
[73088.086989] kdev: Encrypt set public key ok
[73088.087410] Using cipher: sm2
[73088.087761] Encrypting 10 bytes with public key
[73088.088192] PLAINTEXT: 00000000: 6c 69 6e 75 78 2d 6b 64 65 76                    linux-kdev
[73088.088839] Public key encryption failed: -38
```


内核 SM2 实现现状：

Linux 内核的 SM2 实现主要专注于签名/验签功能（用于 IMA、模块签名等）

加密/解密功能在内核中有代码但未完全集成到加密框架中

crypto_akcipher_encrypt/decrypt 操作未绑定到 SM2 算法

错误原因：

```
[73088.088839] Public key encryption failed: -38
```

* -38 (ENOSYS) 表示内核的 SM2 实现未提供加密操作的回调函数

