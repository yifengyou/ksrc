#include <linux/module.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/completion.h>
#include <crypto/internal/akcipher.h>

#define DRIVER_AUTHOR "Your Name <your.email@example.com>"
#define DRIVER_DESC   "SM2 Signature Verification Module"

// 自定义完成结构用于异步操作
struct sm2_verify_completion {
	int result;
	struct completion completion;
};

// 异步操作完成回调函数
static void sm2_verify_done(void *req, int err)
{
	struct sm2_verify_completion *comp = ((struct crypto_async_request *)req)->data;

	if (err == -EINPROGRESS)
		return;

	comp->result = err;
	complete(&comp->completion);
}

// SM2签名验证函数
static int sm2_verify_signature(const u8 *pub_key, size_t pub_key_len,
                                const u8 *message, size_t msg_len,
                                const u8 *signature, size_t sig_len)
{
	struct crypto_akcipher *tfm;
	struct akcipher_request *req;
	struct scatterlist src_tab[2];
	struct sm2_verify_completion comp;
	int ret;

	// 分配SM2算法句柄
	tfm = crypto_alloc_akcipher("sm2", 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("Failed to allocate SM2 cipher handle\n");
		return PTR_ERR(tfm);
	}

	// 设置公钥
	ret = crypto_akcipher_set_pub_key(tfm, pub_key, pub_key_len);
	if (ret) {
		pr_err("SM2 public key setting failed: %d\n", ret);
		goto free_tfm;
	}

	// 分配请求结构
	req = akcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto free_tfm;
	}

	// 初始化完成结构
	comp.result = -EINVAL;
	init_completion(&comp.completion);

	// 设置分散列表
	sg_init_table(src_tab, 2);
	sg_set_buf(&src_tab[0], message, msg_len);
	sg_set_buf(&src_tab[1], signature, sig_len);

	// 设置异步请求参数
	akcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
	                              sm2_verify_done, &comp);
	akcipher_request_set_crypt(req, src_tab, NULL, msg_len, sig_len);

	// 执行验证操作
	ret = crypto_akcipher_verify(req);
	if (ret == -EINPROGRESS || ret == -EBUSY) {
		// 等待异步操作完成
		wait_for_completion(&comp.completion);
		ret = comp.result;
	}

	if (ret == 0) {
		pr_info("SM2 signature verification succeeded\n");
	} else {
		pr_info("SM2 signature verification failed: %d\n", ret);
	}

	// 清理资源
	akcipher_request_free(req);
free_tfm:
	crypto_free_akcipher(tfm);
	return ret;
}

// 示例数据 - 实际应用中应从外部获取
static const u8 test_pub_key[65] = {
	0x04, // 未压缩格式标识
	// 以下是示例公钥 (x, y坐标) - 实际应用中需替换为真实密钥
	0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, // x坐标 (32字节)
	0x39, 0x30, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
	0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x30, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
	0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, // y坐标 (32字节)
	0x39, 0x30, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
	0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x30, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46
};

static const char test_message[] = "Test message for SM2 verification";
static const u8 test_signature[64] = {
	// 以下是示例签名 (r, s值) - 实际应用中需替换为真实签名
	0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, // r值 (32字节)
	0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
	0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
	0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
	0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, // s值 (32字节)
	0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
	0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
	0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99
};

// 模块初始化函数
static int __init sm2_verify_init(void)
{
	int ret;

	pr_info("SM2 Verification Module Loaded\n");

	// 执行SM2签名验证
	ret = sm2_verify_signature(test_pub_key, sizeof(test_pub_key),
	                           (const u8 *)test_message, strlen(test_message),
	                           test_signature, sizeof(test_signature));

	// 实际应用中应检查返回值并处理错误
	return 0;
}

// 模块退出函数
static void __exit sm2_verify_exit(void)
{
	pr_info("SM2 Verification Module Unloaded\n");
}

module_init(sm2_verify_init);
module_exit(sm2_verify_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION("1.0");