#!/bin/bash
#
# openssl version
# OpenSSL 3.0.12 24 Oct 2023 (Library: OpenSSL 3.0.12 24 Oct 2023)
# 

set -xe

openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out private_key_pkcs1.pem

openssl rsa -in private_key_pkcs1.pem -pubout -out public_key_pkcs1.pem

openssl rsa -in private_key_pkcs1.pem -traditional -outform DER -out private.der
openssl rsa -in private.der -inform der -text -noout


openssl rsa -pubin -in public_key_pkcs1.pem -RSAPublicKey_out -outform DER -out public.der
openssl rsa -pubin -in public.der -inform der -text -noout


xxd -i private.der > private.h
xxd -i public.der > public.h


echo hello > plain.txt
openssl pkeyutl -encrypt -inkey public.der -in plain.txt -pubin -out cipher.bin -keyform DER
openssl pkeyutl -decrypt -inkey private.der -in cipher.bin -out decrypted.txt -keyform DER
diff decrypted.txt plain.txt
if [ $? -ne 0 ] ; then
	echo "kdev: fatal error! test failed!"
	exit 1
fi
echo "kdev: public key/private key test ok!"
rm -f cipher.bin decrypted.txt plain.txt

exit 0

