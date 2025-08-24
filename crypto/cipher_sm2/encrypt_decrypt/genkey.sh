#!/bin/bash
#
# openssl version
# OpenSSL 3.0.12 24 Oct 2023 (Library: OpenSSL 3.0.12 24 Oct 2023)
# 

set -xe

openssl ecparam -name sm2 -genkey -out sm2_private_key.pem
openssl ec -in sm2_private_key.pem -pubout -out sm2_public_key.pem

openssl ec -in sm2_private_key.pem -outform der -out sm2_private_key.der
openssl ec -in sm2_private_key.pem -pubout -outform der -out sm2_public_key.der

xxd -i sm2_private_key.der > private.h
xxd -i sm2_public_key.der > public.h


openssl asn1parse -inform DER -in sm2_public_key.der
openssl asn1parse -inform DER -in sm2_private_key.der

echo hello > plain.txt
openssl pkeyutl -encrypt -inkey sm2_public_key.der -in plain.txt -pubin -out cipher.bin -keyform DER
openssl pkeyutl -decrypt -inkey sm2_private_key.der -in cipher.bin -out decrypted.txt -keyform DER
cat decrypted.txt
diff decrypted.txt plain.txt
if [ $? -ne 0 ] ; then
	echo "kdev: fatal error! test failed!"
	exit 1
fi

echo "kdev: public key/private key test ok!"
rm -f cipher.bin decrypted.txt plain.txt

exit 0

