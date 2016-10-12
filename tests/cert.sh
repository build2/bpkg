#! /bin/sh

# Normally, you don't need to regenerate the private key.
#
# openssl genrsa 4096 > key.pem

# Copy default-cert.pem content to the certificate value of the following
# manifest files:
#   pkg/1/build2.org/auth/signature-mismatch/repositories
#   pkg/1/build2.org/auth/sha256sum-mismatch/repositories
#   pkg/1/build2.org/auth/signed/repositories
#   pkg/1/build2.org/common/hello/repositories
#
openssl req -x509 -new -key key.pem -days 365 -config default-openssl.cnf > \
	default-cert.pem

# Copy mismatch-cert.pem content to the certificate value of
# pkg/1/build2.org/auth/name-mismatch/repositories manifest file.
#
openssl req -x509 -new -key key.pem -days 365 -config mismatch-openssl.cnf > \
        mismatch-cert.pem

# Copy noemail-cert.pem content to the certificate value of
# pkg/1/build2.org/auth/create-noemail/repositories manifest file.
#
openssl req -x509 -new -key key.pem -days 365 -config noemail-openssl.cnf > \
        noemail-cert.pem

# Normally, you have no reason to regenerate expired-cert.pem, as need to keep
# it expired for the testing purposes. But if you do, copy expired-cert.pem
# content to the certificate value of the following manifest files:
#   pkg/1/build2.org/auth/expired/repositories
#   pkg/1/build2.org/auth/create-expired/repositories
#
# To regenerate the packages and signature manifest files run:
#
# ../bpkg/bpkg rep-create pkg/1/build2.org/auth/expired --key key.pem
#
# We cannot do it in test.sh since the certificate has expired. This is also
# the reason why we store these auto-generated manifests in git.
#
# Will have to wait 1 day until the certificate expires. Until then test.sh
# will be failing.
#
# openssl req -x509 -new -key key.pem -days 1 -config default-openssl.cnf > \
#         expired-cert.pem
